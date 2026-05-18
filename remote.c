/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Sam Dorey
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "remote.h"

struct remote_hosts remote_hosts = TAILQ_HEAD_INITIALIZER(remote_hosts);

static void	remote_update_cb(struct job *);
static void	remote_complete_cb(struct job *);
static void	remote_free_cb(void *);
static void	remote_parse_line(struct remote_host *, const char *);
static void	remote_parse_output(struct remote_host *, const char *);
static void	remote_parse_sessions(struct remote_host *, const char *);
static void	remote_parse_windows(struct remote_host *, const char *);
static void	remote_parse_panes(struct remote_host *, const char *);
static void		remote_spawn_sessions(struct remote_host *);
static void		remote_destroy_sessions(struct remote_host *);
static size_t		remote_decode_output(const char *, u_char *, size_t);
static enum cmd_retval	remote_mark_panes_cb(struct cmdq_item *, void *);
static void		remote_set_session_remote(struct session *,
			    struct remote_host *, const char *);

void
remote_init(void)
{
	TAILQ_INIT(&remote_hosts);
}

void
remote_destroy(void)
{
	struct remote_host	*rh, *rh1;

	TAILQ_FOREACH_SAFE(rh, &remote_hosts, entry, rh1)
		remote_remove(rh);
}

struct remote_host *
remote_add(const char *name, const char *ssh_target, const char *tmux_target)
{
	struct remote_host	*rh;

	rh = xcalloc(1, sizeof *rh);
	rh->name = xstrdup(name);
	rh->ssh_target = xstrdup(ssh_target);
	if (tmux_target != NULL)
		rh->tmux_target = xstrdup(tmux_target);
	rh->state = REMOTE_DISCONNECTED;
	TAILQ_INIT(&rh->sessions);
	rh->pending = evbuffer_new();
	TAILQ_INSERT_TAIL(&remote_hosts, rh, entry);
	return (rh);
}

void
remote_remove(struct remote_host *rh)
{
	remote_disconnect(rh);
	remote_destroy_sessions(rh);
	remote_clear_tree(rh);
	TAILQ_REMOVE(&remote_hosts, rh, entry);
	evbuffer_free(rh->pending);
	free(rh->name);
	free(rh->ssh_target);
	free(rh->tmux_target);
	free(rh->error);
	free(rh);
}

struct remote_host *
remote_find(const char *name)
{
	struct remote_host	*rh;

	TAILQ_FOREACH(rh, &remote_hosts, entry) {
		if (strcmp(rh->name, name) == 0)
			return (rh);
	}
	return (NULL);
}

void
remote_clear_tree(struct remote_host *rh)
{
	struct remote_session	*rs, *rs1;
	struct remote_window	*rw, *rw1;
	struct remote_pane	*rp, *rp1;

	TAILQ_FOREACH_SAFE(rs, &rh->sessions, entry, rs1) {
		TAILQ_FOREACH_SAFE(rw, &rs->windows, entry, rw1) {
			TAILQ_FOREACH_SAFE(rp, &rw->panes, entry, rp1) {
				TAILQ_REMOVE(&rw->panes, rp, entry);
				free(rp->title);
				free(rp);
			}
			TAILQ_REMOVE(&rs->windows, rw, entry);
			free(rw->name);
			free(rw);
		}
		TAILQ_REMOVE(&rh->sessions, rs, entry);
		free(rs->name);
		free(rs);
	}
}

char *
remote_control_path(struct remote_host *rh)
{
	char	*path;

	xasprintf(&path, "/tmp/tmux-remote-%s", rh->name);
	return (path);
}

void
remote_connect(struct remote_host *rh, struct cmdq_item *item)
{
	struct client		*tc;
	struct session		*s;
	struct spawn_context	 sc;
	struct winlink		*new_wl;
	char			*cause = NULL;
	char			*wname, *ctrl_path, *cmd;

	if (rh->state == REMOTE_CONNECTING || rh->state == REMOTE_READY)
		return;

	free(rh->error);
	rh->error = NULL;
	rh->state = REMOTE_CONNECTING;

	tc = cmdq_get_target_client(item);
	if (tc == NULL || tc->session == NULL) {
		remote_connect_control(rh);
		return;
	}
	s = tc->session;

	ctrl_path = remote_control_path(rh);

	xasprintf(&cmd,
	    "ssh -o 'ControlPath=%s' -O check %s 2>/dev/null && { "
	    "  tmux remote-refresh %s; exit 0; }; "
	    "ssh -o 'ControlPath=%s' -O exit %s 2>/dev/null; "
	    "rm -f '%s'; "
	    "ssh -o ControlMaster=yes -o 'ControlPath=%s' "
	    "-o ControlPersist=600 %s true && "
	    "tmux remote-refresh %s || { "
	    "  echo 'remote-add: auth failed. Press Enter to close.'; read; }",
	    ctrl_path, rh->ssh_target, rh->name,
	    ctrl_path, rh->ssh_target,
	    ctrl_path,
	    ctrl_path, rh->ssh_target, rh->name);

	memset(&sc, 0, sizeof sc);
	sc.item = item;
	sc.s = s;
	sc.tc = tc;
	sc.argc = 3;
	sc.argv = xcalloc(3, sizeof *sc.argv);
	sc.argv[0] = xstrdup("/bin/sh");
	sc.argv[1] = xstrdup("-c");
	sc.argv[2] = xstrdup(cmd);
	sc.environ = environ_create();
	xasprintf(&wname, "[auth:%s]", rh->name);
	sc.name = wname;
	sc.idx = -1;
	sc.cwd = NULL;
	sc.flags = 0;

	new_wl = spawn_window(&sc, &cause);
	if (new_wl == NULL) {
		cmdq_error(item, "spawn auth window failed: %s", cause);
		free(cause);
		rh->state = REMOTE_FAILED;
		rh->error = xstrdup("failed to open auth window");
	}

	cmd_free_argv(sc.argc, sc.argv);
	environ_free(sc.environ);
	free(wname);
	free(ctrl_path);
	free(cmd);
}

void
remote_connect_control(struct remote_host *rh)
{
	char	*cmd, *ctrl_path;

	if (rh->state == REMOTE_READY)
		return;

	if (rh->state != REMOTE_CONNECTING) {
		free(rh->error);
		rh->error = NULL;
		rh->state = REMOTE_CONNECTING;
	}

	ctrl_path = remote_control_path(rh);

	/*
	 * Use new-session -A -s <name> so the control client either attaches
	 * to an existing session or creates one. This ensures %output flows
	 * for that session's panes (control mode only sends %output for panes
	 * in the attached session).
	 */
	if (rh->tmux_target != NULL)
		xasprintf(&cmd,
		    "ssh -o 'ControlPath=%s' -o ControlMaster=auto "
		    "%s tmux -C new-session -A -s %s",
		    ctrl_path, rh->ssh_target, rh->tmux_target);
	else
		xasprintf(&cmd,
		    "ssh -o 'ControlPath=%s' -o ControlMaster=auto "
		    "%s tmux -C new-session -A -s main",
		    ctrl_path, rh->ssh_target);

	rh->job = job_run(cmd, 0, NULL, NULL, NULL, NULL,
	    remote_update_cb, remote_complete_cb, remote_free_cb,
	    rh, JOB_NOWAIT | JOB_KEEPWRITE, -1, -1);
	free(cmd);
	free(ctrl_path);
}

void
remote_disconnect(struct remote_host *rh)
{
	if (rh->job != NULL) {
		job_free(rh->job);
		rh->job = NULL;
	}
	rh->state = REMOTE_DISCONNECTED;
	evbuffer_drain(rh->pending, evbuffer_get_length(rh->pending));
}

void
remote_refresh(struct remote_host *rh)
{
	struct bufferevent	*bev;

	if (rh->state != REMOTE_READY || rh->job == NULL)
		return;

	bev = job_get_event(rh->job);
	if (bev == NULL)
		return;

	remote_destroy_sessions(rh);
	remote_clear_tree(rh);
	rh->parse_state = PARSE_SESSIONS;

	bufferevent_write(bev, "list-sessions -F "
	    "'#{session_id}:#{session_name}:#{session_attached}'\n",
	    strlen("list-sessions -F "
	    "'#{session_id}:#{session_name}:#{session_attached}'\n"));
	bufferevent_write(bev, "list-windows -a -F "
	    "'#{session_id}:#{window_id}:#{window_index}:"
	    "#{window_name}:#{window_active}'\n",
	    strlen("list-windows -a -F "
	    "'#{session_id}:#{window_id}:#{window_index}:"
	    "#{window_name}:#{window_active}'\n"));
	bufferevent_write(bev, "list-panes -a -F "
	    "'#{window_id}:#{pane_id}:#{pane_title}:#{pane_active}'\n",
	    strlen("list-panes -a -F "
	    "'#{window_id}:#{pane_id}:#{pane_title}:#{pane_active}'\n"));
}

static void
remote_update_cb(struct job *job)
{
	struct remote_host	*rh = job_get_data(job);
	struct bufferevent	*bev = job_get_event(job);
	struct evbuffer		*buf;
	char			*line;

	if (bev == NULL)
		return;
	buf = bufferevent_get_input(bev);

	while ((line = evbuffer_readln(buf, NULL, EVBUFFER_EOL_LF)) != NULL) {
		remote_parse_line(rh, line);
		free(line);
	}
}

static void
remote_complete_cb(struct job *job)
{
	struct remote_host	*rh = job_get_data(job);
	int			 status = job_get_status(job);

	rh->job = NULL;
	if (status != 0) {
		rh->state = REMOTE_FAILED;
		free(rh->error);
		xasprintf(&rh->error, "ssh exited with status %d", status);
	} else {
		rh->state = REMOTE_DISCONNECTED;
	}
}

static void
remote_free_cb(void *data __attribute__((unused)))
{
}

/*
 * Parse control mode output. Handle %output for proxy pane I/O,
 * %begin/%end for command responses, and ignore other notifications.
 */
static void
remote_parse_line(struct remote_host *rh, const char *line)
{
	/* Handle %output — real-time pane output for proxy panes. */
	if (strncmp(line, "%output ", 8) == 0) {
		remote_parse_output(rh, line + 8);
		return;
	}

	if (strncmp(line, "%begin ", 7) == 0)
		return;

	if (strncmp(line, "%end ", 5) == 0) {
		if (rh->state == REMOTE_CONNECTING) {
			rh->state = REMOTE_READY;
			remote_clear_tree(rh);
			rh->parse_state = PARSE_IDLE;
			remote_refresh(rh);
			return;
		}
		switch (rh->parse_state) {
		case PARSE_IDLE:
			break;
		case PARSE_SESSIONS:
			rh->parse_state = PARSE_WINDOWS;
			break;
		case PARSE_WINDOWS:
			rh->parse_state = PARSE_PANES;
			break;
		case PARSE_PANES:
			rh->parse_state = PARSE_IDLE;
			remote_spawn_sessions(rh);
			break;
		}
		return;
	}

	if (strncmp(line, "%error ", 7) == 0) {
		rh->parse_state = PARSE_IDLE;
		return;
	}
	if (strncmp(line, "%exit", 5) == 0) {
		struct window_pane	*ewp;

		/* Kill all proxy panes for this remote. */
		RB_FOREACH(ewp, window_pane_tree, &all_window_panes) {
			if ((ewp->flags & PANE_REMOTE) &&
			    ewp->remote == rh &&
			    ewp->pid > 1)
				kill(ewp->pid, SIGKILL);
		}
		rh->state = REMOTE_DISCONNECTED;
		return;
	}

	/*
	 * Handle window close notifications. tmux sends:
	 * - %unlinked-window-close @<id> when a window closes
	 * - %window-close @<id> (older versions)
	 * Kill local proxy panes for the closed window.
	 */
	if (strncmp(line, "%unlinked-window-close @", 24) == 0 ||
	    strncmp(line, "%window-close @", 15) == 0) {
		u_int			 closed_wid;
		struct remote_session	*rs;
		struct remote_window	*rw;
		struct remote_pane	*rp;
		struct window_pane	*cwp;
		const char		*at;

		at = strchr(line, '@');
		if (at != NULL && sscanf(at, "@%u", &closed_wid) == 1) {
			TAILQ_FOREACH(rs, &rh->sessions, entry) {
				TAILQ_FOREACH(rw, &rs->windows, entry) {
					if (rw->id != closed_wid)
						continue;
					TAILQ_FOREACH(rp, &rw->panes, entry) {
						cwp = remote_find_proxy_pane(
						    rh, rp->id);
						if (cwp != NULL &&
						    cwp->pid > 1) {
							kill(cwp->pid, SIGKILL);
							log_debug("remote: "
							    "killed %%%u "
							    "(window @%u "
							    "closed)",
							    cwp->id,
							    closed_wid);
						}
					}
				}
			}
		}
		return;
	}

	/* Ignore other %-prefixed notifications. */
	if (line[0] == '%')
		return;

	/* Data line — parse based on current state. */
	switch (rh->parse_state) {
	case PARSE_IDLE:
		break;
	case PARSE_SESSIONS:
		remote_parse_sessions(rh, line);
		break;
	case PARSE_WINDOWS:
		remote_parse_windows(rh, line);
		break;
	case PARSE_PANES:
		remote_parse_panes(rh, line);
		break;
	}
}

/*
 * Handle %output %<pane_id> <octal-escaped-data>.
 * Decode the data and inject it into the corresponding local proxy pane
 * via input_parse_buffer(), which feeds the terminal emulator directly.
 */
static void
remote_parse_output(struct remote_host *rh, const char *line)
{
	u_int			 pane_id;
	const char		*data;
	struct window_pane	*wp;
	u_char			*buf;
	size_t			 len;

	/* Parse: %<pane_id> <data> */
	if (line[0] != '%')
		return;
	if (sscanf(line, "%%%u", &pane_id) != 1)
		return;

	data = strchr(line, ' ');
	if (data == NULL)
		return;
	data++; /* skip space */

	/* Find the local proxy pane for this remote pane. */
	wp = remote_find_proxy_pane(rh, pane_id);
	if (wp == NULL) {
		/*
		 * No mapped pane — look for an unmapped proxy pane
		 * (remote_pane == UINT_MAX) and assign this remote pane
		 * to it. This handles panes created by prefix+c where
		 * we don't know the remote ID until output arrives.
		 */
		RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
			if ((wp->flags & PANE_REMOTE) &&
			    wp->remote == rh &&
			    wp->remote_pane == UINT_MAX) {
				wp->remote_pane = pane_id;
				log_debug("remote: auto-mapped %%%u -> "
				    "remote %%%u", wp->id, pane_id);
				break;
			}
		}
		if (wp == NULL)
			return;
	}

	/* Decode octal escapes and inject into the pane's input parser. */
	buf = xmalloc(strlen(data) + 1);
	len = remote_decode_output(data, buf, strlen(data) + 1);
	if (len > 0) {
		input_parse_buffer(wp, buf, len);
		wp->flags |= PANE_CHANGED;
	}
	free(buf);
}

/*
 * Decode control mode octal-escaped output.
 * Characters < 0x20 and backslash are encoded as \NNN (octal).
 * Returns number of decoded bytes.
 */
static size_t
remote_decode_output(const char *in, u_char *out, size_t outsize)
{
	size_t	len = 0;

	while (*in != '\0' && len < outsize - 1) {
		if (in[0] == '\\' && in[1] >= '0' && in[1] <= '3' &&
		    in[2] >= '0' && in[2] <= '7' &&
		    in[3] >= '0' && in[3] <= '7') {
			out[len++] = ((in[1] - '0') << 6) |
			    ((in[2] - '0') << 3) | (in[3] - '0');
			in += 4;
		} else {
			out[len++] = *in++;
		}
	}
	return (len);
}

/* Parse: $id:name:attached */
static void
remote_parse_sessions(struct remote_host *rh, const char *line)
{
	struct remote_session	*rs;
	u_int			 id, attached;
	char			 name[256];

	if (sscanf(line, "$%u:%255[^:]:%u", &id, name, &attached) != 3)
		return;

	rs = xcalloc(1, sizeof *rs);
	rs->id = id;
	rs->name = xstrdup(name);
	rs->attached = attached;
	TAILQ_INIT(&rs->windows);
	TAILQ_INSERT_TAIL(&rh->sessions, rs, entry);
}

/* Parse: $session_id:@window_id:index:name:active */
static void
remote_parse_windows(struct remote_host *rh, const char *line)
{
	struct remote_session	*rs;
	struct remote_window	*rw;
	u_int			 sid, wid, idx, active;
	char			 name[256];

	if (sscanf(line, "$%u:@%u:%u:%255[^:]:%u",
	    &sid, &wid, &idx, name, &active) != 5)
		return;

	TAILQ_FOREACH(rs, &rh->sessions, entry) {
		if (rs->id == sid)
			break;
	}
	if (rs == NULL)
		return;

	rw = xcalloc(1, sizeof *rw);
	rw->id = wid;
	rw->idx = idx;
	rw->name = xstrdup(name);
	rw->active = active;
	TAILQ_INIT(&rw->panes);
	TAILQ_INSERT_TAIL(&rs->windows, rw, entry);
}

/* Parse: @window_id:%pane_id:title:active */
static void
remote_parse_panes(struct remote_host *rh, const char *line)
{
	struct remote_session	*rs;
	struct remote_window	*rw;
	struct remote_pane	*rp;
	u_int			 wid, pid, active;
	char			 title[256];

	if (sscanf(line, "@%u:%%%u:%255[^:]:%u",
	    &wid, &pid, title, &active) != 4)
		return;

	TAILQ_FOREACH(rs, &rh->sessions, entry) {
		TAILQ_FOREACH(rw, &rs->windows, entry) {
			if (rw->id == wid)
				goto found;
		}
	}
	return;

found:
	rp = xcalloc(1, sizeof *rp);
	rp->id = pid;
	rp->title = xstrdup(title);
	rp->active = active;
	TAILQ_INSERT_TAIL(&rw->panes, rp, entry);
}

/*
 * Create local sessions mirroring the remote tree.
 *
 * Sessions are created via queued commands. Once created, the
 * remote_mark_panes_cb callback marks the session as remote, which
 * causes spawn_pane() to automatically create proxy panes for any
 * new windows/panes in that session (including the initial one).
 *
 * However, since the initial pane is created by new-session before
 * we can mark the session as remote, we use the callback to also
 * retroactively mark existing panes.
 */
static void
remote_spawn_sessions(struct remote_host *rh)
{
	struct remote_session	*rs;
	struct remote_window	*rw;
	struct remote_pane	*rp;
	char			*sname, *cmd, *error;
	struct cmdq_state	*state;
	enum cmd_parse_status	 status;
	int			 first_win;

	TAILQ_FOREACH(rs, &rh->sessions, entry) {
		xasprintf(&sname, "%s/%s", rh->name, rs->name);

		if (session_find(sname) != NULL) {
			free(sname);
			continue;
		}

		first_win = 1;
		TAILQ_FOREACH(rw, &rs->windows, entry) {
			rp = TAILQ_FIRST(&rw->panes);
			if (rp == NULL)
				continue;

			if (first_win) {
				xasprintf(&cmd,
				    "new-session -d -s '%s' -n '%s' -x 80 -y 24",
				    sname, rw->name);
				first_win = 0;
			} else {
				xasprintf(&cmd,
				    "new-window -d -t '%s:' -n '%s'",
				    sname, rw->name);
			}

			state = cmdq_new_state(NULL, NULL, 0);
			status = cmd_parse_and_append(cmd, NULL, NULL,
			    state, &error);
			if (status == CMD_PARSE_ERROR) {
				log_debug("remote: %s: %s", sname, error);
				free(error);
			}
			cmdq_free_state(state);
			free(cmd);
		}

		if (first_win) {
			free(sname);
			continue;
		}

		/*
		 * Set session options.
		 */
		xasprintf(&cmd,
		    "set-option -t '%s' detach-on-destroy no-detached",
		    sname);
		state = cmdq_new_state(NULL, NULL, 0);
		cmd_parse_and_append(cmd, NULL, NULL, state, &error);
		cmdq_free_state(state);
		free(cmd);

		log_debug("remote: queued session %s", sname);
		free(sname);
	}

	/* After sessions are created, mark them as remote. */
	{
		struct cmdq_item *cb_item;
		cb_item = cmdq_get_callback(remote_mark_panes_cb, rh);
		cmdq_append(NULL, cb_item);
	}
}

/*
 * Callback that runs after queued new-session commands. Marks each
 * session as remote (so spawn_pane handles future panes natively)
 * and retroactively marks existing panes as PANE_REMOTE.
 */
static enum cmd_retval
remote_mark_panes_cb(__unused struct cmdq_item *item, void *data)
{
	struct remote_host	*rh = data;
	struct remote_session	*rs;
	struct remote_window	*rw;
	struct remote_pane	*rp;
	struct session		*s;
	struct winlink		*wl;
	struct window_pane	*wp;
	char			*sname;

	TAILQ_FOREACH(rs, &rh->sessions, entry) {
		xasprintf(&sname, "%s/%s", rh->name, rs->name);
		s = session_find(sname);
		free(sname);
		if (s == NULL)
			continue;

		/* Mark the session itself as remote. */
		remote_set_session_remote(s, rh, rs->name);

		/*
		 * Retroactively mark existing panes as PANE_REMOTE.
		 * Match remote windows to local windows in order.
		 */
		wl = RB_MIN(winlinks, &s->windows);
		TAILQ_FOREACH(rw, &rs->windows, entry) {
			rp = TAILQ_FIRST(&rw->panes);
			if (rp == NULL)
				continue;
			if (wl == NULL)
				break;

			wp = wl->window->active;
			if (wp != NULL) {
				wp->flags |= PANE_REMOTE;
				wp->remote = rh;
				wp->remote_pane = rp->id;
				log_debug("remote: marked %%%u -> remote %%%u",
				    wp->id, rp->id);
			}
			wl = RB_NEXT(winlinks, &s->windows, wl);
		}
	}

	/* Trigger initial screen content for all proxy panes. */
	{
		struct bufferevent *bev = job_get_event(rh->job);
		if (bev != NULL) {
			char cmd[64];
			TAILQ_FOREACH(rs, &rh->sessions, entry) {
				TAILQ_FOREACH(rw, &rs->windows, entry) {
					rp = TAILQ_FIRST(&rw->panes);
					if (rp == NULL)
						continue;
					snprintf(cmd, sizeof cmd,
					    "send-keys -t %%%u ''\n", rp->id);
					bufferevent_write(bev, cmd,
					    strlen(cmd));
				}
			}
		}
	}

	return (CMD_RETURN_NORMAL);
}

/*
 * Mark a session as remote. Future spawn_pane() calls in this session
 * will automatically create proxy panes.
 */
static void
remote_set_session_remote(struct session *s, struct remote_host *rh,
    const char *remote_session_name)
{
	s->remote = rh;
	free(s->remote_session);
	s->remote_session = xstrdup(remote_session_name);
	log_debug("remote: session %s -> %s/%s", s->name, rh->name,
	    remote_session_name);
}

static void
remote_destroy_sessions(struct remote_host *rh)
{
	struct session	*s, *s1;
	char		*prefix;
	size_t		 prefixlen;

	xasprintf(&prefix, "%s/", rh->name);
	prefixlen = strlen(prefix);

	RB_FOREACH_SAFE(s, sessions, &sessions, s1) {
		if (strncmp(s->name, prefix, prefixlen) == 0) {
			log_debug("remote: destroying session %s", s->name);
			server_destroy_session(s);
			session_destroy(s, 1, __func__);
		}
	}
	free(prefix);
}

/*
 * Find a local proxy pane that corresponds to a remote pane ID.
 */
struct window_pane *
remote_find_proxy_pane(struct remote_host *rh, u_int remote_pane_id)
{
	struct window_pane	*wp;

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if ((wp->flags & PANE_REMOTE) &&
		    wp->remote == rh &&
		    wp->remote_pane == remote_pane_id)
			return (wp);
	}
	return (NULL);
}

/*
 * Create a new window on the remote tmux via control mode.
 * Called from spawn_pane() when a new pane is created in a remote session.
 */
void
remote_create_window(struct remote_host *rh, const char *remote_session)
{
	struct bufferevent	*bev;
	char			 cmd[256];

	if (rh->state != REMOTE_READY || rh->job == NULL)
		return;

	bev = job_get_event(rh->job);
	if (bev == NULL)
		return;

	snprintf(cmd, sizeof cmd, "new-window -t '%s'\n", remote_session);
	bufferevent_write(bev, cmd, strlen(cmd));
	log_debug("remote: created window on %s/%s", rh->name, remote_session);
}

/*
 * Send a keystroke to a remote pane via the control mode connection.
 */
void
remote_send_key(struct window_pane *wp, key_code key,
    __unused struct mouse_event *m)
{
	struct remote_host	*rh = wp->remote;
	struct bufferevent	*bev;
	const char		*keystr;
	char			 cmd[512];
	u_char			 ch;

	if (rh == NULL || rh->job == NULL || rh->state != REMOTE_READY)
		return;
	if (wp->remote_pane == UINT_MAX)
		return; /* not mapped yet */

	bev = job_get_event(rh->job);
	if (bev == NULL)
		return;

	/* Ignore mouse events for now. */
	if (KEYC_IS_MOUSE(key))
		return;

	/*
	 * For simple ASCII characters, send them as literal text which is
	 * more reliable than key names for things like quotes, semicolons.
	 */
	if (key < 0x80 && key >= 0x20) {
		ch = (u_char)key;
		/* Escape single quotes for the shell. */
		if (ch == '\'')
			snprintf(cmd, sizeof cmd,
			    "send-keys -t %%%u -l \"'\"\n", wp->remote_pane);
		else if (ch == '\\')
			snprintf(cmd, sizeof cmd,
			    "send-keys -t %%%u -l '\\\\'\n", wp->remote_pane);
		else
			snprintf(cmd, sizeof cmd,
			    "send-keys -t %%%u -l '%c'\n", wp->remote_pane, ch);
		bufferevent_write(bev, cmd, strlen(cmd));
		return;
	}

	/* For control characters (Ctrl+C, Enter, etc.), use key names. */
	keystr = key_string_lookup_key(key, 0);
	if (keystr == NULL || *keystr == '\0')
		return;

	snprintf(cmd, sizeof cmd, "send-keys -t %%%u %s\n",
	    wp->remote_pane, keystr);
	bufferevent_write(bev, cmd, strlen(cmd));
}

void
remote_open(struct remote_host *rh, const char *target, struct cmdq_item *item)
{
	struct session	*s;
	struct client	*tc;
	char		*sname;

	if (target != NULL)
		xasprintf(&sname, "%s/%s", rh->name, target);
	else {
		struct remote_session *rs = TAILQ_FIRST(&rh->sessions);
		if (rs == NULL) {
			cmdq_error(item, "no sessions on remote %s", rh->name);
			return;
		}
		xasprintf(&sname, "%s/%s", rh->name, rs->name);
	}

	s = session_find(sname);
	if (s == NULL) {
		cmdq_error(item, "session not found: %s", sname);
		free(sname);
		return;
	}

	tc = cmdq_get_target_client(item);
	if (tc == NULL) {
		cmdq_error(item, "no client");
		free(sname);
		return;
	}

	server_client_set_session(tc, s);
	free(sname);
}
