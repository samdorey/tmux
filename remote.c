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
static void	remote_parse_line(struct remote_conn *, const char *);
static struct remote_conn *remote_spawn_conn(struct remote_host *, const char *,
		    int);
static void	remote_conn_free(struct remote_conn *);
static struct remote_conn *remote_find_conn(struct remote_host *, const char *);
static struct bufferevent *remote_primary_bev(struct remote_host *);
static void	remote_spawn_session_conns(struct remote_host *);
static void	remote_kill_session_panes(struct remote_host *, const char *);
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
static void		remote_queue_command(const char *);
static u_int		remote_layout_pane_ids(const char *, u_int *, u_int);
static enum cmd_retval	remote_apply_layout_cb(struct cmdq_item *, void *);

/* Pending layout application, run as a queued callback. */
struct remote_layout_apply {
	struct remote_host	*rh;
	u_int			 local_window;	/* local @id */
	u_int			 ids[256];	/* remote pane ids, cell order */
	u_int			 n;
	int			 dec_mirroring;	/* undo rh->mirroring bump */
};

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
	TAILQ_INIT(&rh->conns);
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
			free(rw->layout);
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

/*
 * Spawn one control-mode connection. If session is non-NULL it attaches to
 * that specific session; if NULL (the primary with no configured target) it
 * attaches to the remote's most-recent session, and its streamed session is
 * resolved to the first discovered one later (see remote_spawn_sessions).
 *
 * attach-session never creates a session, so mounting a host no longer
 * spawns a spurious "main". All connections share the host's SSH
 * ControlMaster, so only the first pays for an SSH handshake.
 */
static struct remote_conn *
remote_spawn_conn(struct remote_host *rh, const char *session, int primary)
{
	struct remote_conn	*rc;
	char			*cmd, *ctrl_path;

	rc = xcalloc(1, sizeof *rc);
	rc->rh = rh;
	rc->session = (session != NULL) ? xstrdup(session) : NULL;
	rc->primary = primary;
	rc->state = REMOTE_CONNECTING;
	rc->parse_state = PARSE_IDLE;
	TAILQ_INSERT_TAIL(&rh->conns, rc, entry);

	ctrl_path = remote_control_path(rh);
	if (session != NULL)
		xasprintf(&cmd,
		    "ssh -o 'ControlPath=%s' -o ControlMaster=auto "
		    "%s tmux -C attach-session -t %s",
		    ctrl_path, rh->ssh_target, session);
	else
		xasprintf(&cmd,
		    "ssh -o 'ControlPath=%s' -o ControlMaster=auto "
		    "%s tmux -C attach-session",
		    ctrl_path, rh->ssh_target);

	rc->job = job_run(cmd, 0, NULL, NULL, NULL, NULL,
	    remote_update_cb, remote_complete_cb, remote_free_cb,
	    rc, JOB_NOWAIT | JOB_KEEPWRITE, -1, -1);
	free(cmd);
	free(ctrl_path);
	return (rc);
}

static void
remote_conn_free(struct remote_conn *rc)
{
	/*
	 * job_free() invokes remote_free_cb(), which removes rc from the list
	 * and frees it. If the job already finished, free directly.
	 */
	if (rc->job != NULL)
		job_free(rc->job);
	else
		remote_free_cb(rc);
}

/* Find the connection streaming a given remote session. */
static struct remote_conn *
remote_find_conn(struct remote_host *rh, const char *session)
{
	struct remote_conn	*rc;

	TAILQ_FOREACH(rc, &rh->conns, entry) {
		if (rc->session != NULL && strcmp(rc->session, session) == 0)
			return (rc);
	}
	return (NULL);
}

/* The bufferevent of the primary connection, if ready (for sending cmds). */
static struct bufferevent *
remote_primary_bev(struct remote_host *rh)
{
	struct remote_conn	*rc;

	TAILQ_FOREACH(rc, &rh->conns, entry) {
		if (rc->primary && rc->state == REMOTE_READY && rc->job != NULL)
			return (job_get_event(rc->job));
	}
	return (NULL);
}

/*
 * Once the tree is known, ensure every remote session has a streaming
 * connection. The primary already streams its own session; spawn one for
 * each of the others. Streaming the same session twice would duplicate
 * %output, so sessions already covered are skipped.
 */
static void
remote_spawn_session_conns(struct remote_host *rh)
{
	struct remote_session	*rs;

	TAILQ_FOREACH(rs, &rh->sessions, entry) {
		if (remote_find_conn(rh, rs->name) != NULL)
			continue;
		log_debug("remote: streaming conn for %s/%s", rh->name,
		    rs->name);
		remote_spawn_conn(rh, rs->name, 0);
	}
}

void
remote_connect_control(struct remote_host *rh)
{
	if (rh->state == REMOTE_READY)
		return;

	if (rh->state != REMOTE_CONNECTING) {
		free(rh->error);
		rh->error = NULL;
		rh->state = REMOTE_CONNECTING;
	}

	/*
	 * Primary connection: drives discovery and streams one session. With a
	 * configured tmux-target it attaches there; otherwise it attaches to
	 * the most-recent session and is retargeted to the first discovered
	 * one once the tree is known.
	 */
	remote_spawn_conn(rh, rh->tmux_target, 1);
}

void
remote_disconnect(struct remote_host *rh)
{
	struct remote_conn	*rc, *rc1;

	TAILQ_FOREACH_SAFE(rc, &rh->conns, entry, rc1)
		remote_conn_free(rc);
	rh->state = REMOTE_DISCONNECTED;
}

void
remote_refresh(struct remote_host *rh)
{
	struct remote_conn	*rc = NULL, *loop;
	struct bufferevent	*bev;

	/* Discovery runs on the primary connection. */
	TAILQ_FOREACH(loop, &rh->conns, entry) {
		if (loop->primary) {
			rc = loop;
			break;
		}
	}
	if (rc == NULL || rc->state != REMOTE_READY || rc->job == NULL)
		return;

	bev = job_get_event(rc->job);
	if (bev == NULL)
		return;

	remote_destroy_sessions(rh);
	remote_clear_tree(rh);
	rc->parse_state = PARSE_SESSIONS;

	bufferevent_write(bev, "list-sessions -F "
	    "'#{session_id}:#{session_name}:#{session_attached}'\n",
	    strlen("list-sessions -F "
	    "'#{session_id}:#{session_name}:#{session_attached}'\n"));
	bufferevent_write(bev, "list-windows -a -F "
	    "'#{session_id}:#{window_id}:#{window_index}:"
	    "#{window_active}:#{window_layout}:#{window_name}'\n",
	    strlen("list-windows -a -F "
	    "'#{session_id}:#{window_id}:#{window_index}:"
	    "#{window_active}:#{window_layout}:#{window_name}'\n"));
	bufferevent_write(bev, "list-panes -a -F "
	    "'#{window_id}:#{pane_id}:#{pane_title}:#{pane_active}'\n",
	    strlen("list-panes -a -F "
	    "'#{window_id}:#{pane_id}:#{pane_title}:#{pane_active}'\n"));
}

static void
remote_update_cb(struct job *job)
{
	struct remote_conn	*rc = job_get_data(job);
	struct bufferevent	*bev = job_get_event(job);
	struct evbuffer		*buf;
	char			*line;

	if (bev == NULL)
		return;
	buf = bufferevent_get_input(bev);

	while ((line = evbuffer_readln(buf, NULL, EVBUFFER_EOL_LF)) != NULL) {
		remote_parse_line(rc, line);
		free(line);
	}
}

static void
remote_complete_cb(struct job *job)
{
	struct remote_conn	*rc = job_get_data(job);
	struct remote_host	*rh = rc->rh;
	int			 status = job_get_status(job);

	rc->job = NULL;
	rc->state = (status != 0) ? REMOTE_FAILED : REMOTE_DISCONNECTED;

	/* The primary connection's state is the host's state. */
	if (rc->primary) {
		if (status != 0) {
			rh->state = REMOTE_FAILED;
			free(rh->error);
			xasprintf(&rh->error, "ssh exited with status %d",
			    status);
		} else
			rh->state = REMOTE_DISCONNECTED;
	}

	/* rc itself is freed by remote_free_cb, invoked next by job_free(). */
}

static void
remote_free_cb(void *data)
{
	struct remote_conn	*rc = data;

	TAILQ_REMOVE(&rc->rh->conns, rc, entry);
	free(rc->session);
	free(rc);
}

/* Kill the placeholder processes of a remote session's local proxy panes. */
static void
remote_kill_session_panes(struct remote_host *rh, const char *session)
{
	struct session		*s;
	struct winlink		*wl;
	struct window_pane	*wp;
	char			*sname;

	xasprintf(&sname, "%s/%s", rh->name, session);
	s = session_find(sname);
	free(sname);
	if (s == NULL)
		return;

	RB_FOREACH(wl, winlinks, &s->windows) {
		TAILQ_FOREACH(wp, &wl->window->panes, entry) {
			if ((wp->flags & PANE_REMOTE) && wp->pid > 1)
				kill(wp->pid, SIGKILL);
		}
	}
}

/*
 * Parse control mode output. Handle %output for proxy pane I/O,
 * %begin/%end for command responses, and ignore other notifications.
 */
static void
remote_parse_line(struct remote_conn *rc, const char *line)
{
	struct remote_host	*rh = rc->rh;

	/* Handle %output — real-time pane output for proxy panes. */
	if (strncmp(line, "%output ", 8) == 0) {
		remote_parse_output(rh, line + 8);
		return;
	}

	if (strncmp(line, "%begin ", 7) == 0)
		return;

	if (strncmp(line, "%end ", 5) == 0) {
		if (rc->state == REMOTE_CONNECTING) {
			rc->state = REMOTE_READY;
			rc->parse_state = PARSE_IDLE;
			/*
			 * Only the primary connection discovers and builds the
			 * tree; secondary connections just stream their
			 * session's %output.
			 */
			if (rc->primary) {
				rh->state = REMOTE_READY;
				remote_clear_tree(rh);
				remote_refresh(rh);
			}
			return;
		}
		switch (rc->parse_state) {
		case PARSE_IDLE:
			break;
		case PARSE_SESSIONS:
			rc->parse_state = PARSE_WINDOWS;
			break;
		case PARSE_WINDOWS:
			rc->parse_state = PARSE_PANES;
			break;
		case PARSE_PANES:
			rc->parse_state = PARSE_IDLE;
			remote_spawn_sessions(rh);
			break;
		}
		return;
	}

	if (strncmp(line, "%error ", 7) == 0) {
		rc->parse_state = PARSE_IDLE;
		return;
	}
	if (strncmp(line, "%exit", 5) == 0) {
		/*
		 * The session this connection streams has ended. Kill its
		 * proxy panes; the host is only marked down if the primary
		 * connection exits.
		 */
		if (rc->session != NULL)
			remote_kill_session_panes(rh, rc->session);
		if (rc->primary)
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

	/*
	 * Layout change: the remote relayed a window's new geometry (a
	 * resize, split, or pane close). Replicate it locally.
	 * Format: %layout-change @<id> <layout> <visible-layout> <flags>
	 */
	if (strncmp(line, "%layout-change @", 16) == 0) {
		u_int	wid;
		char	layout[2048];

		if (sscanf(line, "%%layout-change @%u %2047s", &wid,
		    layout) == 2)
			remote_apply_layout(rh, wid, layout, 1);
		return;
	}

	/* Ignore other %-prefixed notifications. */
	if (line[0] == '%')
		return;

	/* Data line — parse based on current state. */
	switch (rc->parse_state) {
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

/* Parse: $session_id:@window_id:index:active:layout:name */
static void
remote_parse_windows(struct remote_host *rh, const char *line)
{
	struct remote_session	*rs;
	struct remote_window	*rw;
	u_int			 sid, wid, idx, active;
	char			 layout[2048];
	char			 name[256];

	if (sscanf(line, "$%u:@%u:%u:%u:%2047[^:]:%255[^\n]",
	    &sid, &wid, &idx, &active, layout, name) != 6)
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
	rw->layout = xstrdup(layout);
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

/* Parse and append a tmux command to the global command queue. */
static void
remote_queue_command(const char *cmd)
{
	struct cmdq_state	*state;
	char			*error;

	state = cmdq_new_state(NULL, NULL, 0);
	if (cmd_parse_and_append(cmd, NULL, NULL, state, &error) ==
	    CMD_PARSE_ERROR) {
		log_debug("remote: queue '%s': %s", cmd, error);
		free(error);
	}
	cmdq_free_state(state);
}

/*
 * Extract leaf pane IDs from a window_layout string in layout-tree (cell)
 * order -- the same order layout_parse()/layout_assign() use to assign the
 * window's panes to cells. A leaf cell is "SXxSY,XOFF,YOFF,PANEID"; an
 * internal cell is "SXxSY,XOFF,YOFF" followed by '{' or '['. Returns the
 * number of leaves found (capped at max).
 */
static u_int
remote_layout_pane_ids(const char *layout, u_int *ids, u_int max)
{
	const char	*cp;
	u_int		 n = 0;
	u_int		 sx, sy, xoff, yoff, pid;
	int		 consumed;

	/* Skip the "csum," checksum prefix. */
	cp = strchr(layout, ',');
	if (cp == NULL)
		return (0);
	cp++;

	while (*cp != '\0') {
		if (sscanf(cp, "%ux%u,%u,%u%n", &sx, &sy, &xoff, &yoff,
		    &consumed) == 4) {
			cp += consumed;
			if (*cp == ',') {
				/* Leaf cell: a pane id follows. */
				cp++;
				if (sscanf(cp, "%u%n", &pid, &consumed) == 1) {
					if (n < max)
						ids[n] = pid;
					n++;
					cp += consumed;
				}
			}
			/* Otherwise an internal cell; '{' or '[' follows. */
		} else
			cp++;
	}
	return (n);
}

/*
 * Reconcile a local proxy window with a remote window_layout: adjust the
 * local pane count to match, replicate the geometry, and remap each local
 * pane to its remote pane id. Count changes (kill-pane/split-window) and
 * the select-layout are queued so they run in order; the remap then runs
 * as a trailing callback once those have completed.
 *
 * If allow_spawn is zero, the pane count is left as-is (used for resizes,
 * where only the geometry changes).
 */
void
remote_apply_layout(struct remote_host *rh, u_int wid, const char *layout,
    int allow_spawn)
{
	struct window			*w = NULL, *wsearch;
	struct window_pane		*wp, *wp1;
	struct remote_layout_apply	*rla;
	struct cmdq_item		*item;
	u_int				 ids[256];
	u_int				 n, cur, kept, removed, i;
	char				*cmd;
	int				 found;

	if (layout == NULL || *layout == '\0')
		return;

	/* Find the local proxy window mapped to this remote window. */
	RB_FOREACH(wsearch, windows, &windows) {
		if (wsearch->remote == rh && wsearch->remote_window == wid) {
			w = wsearch;
			break;
		}
	}
	if (w == NULL)
		return;

	n = remote_layout_pane_ids(layout, ids, nitems(ids));
	if (n == 0)
		return;

	rla = xcalloc(1, sizeof *rla);
	rla->rh = rh;
	rla->local_window = w->id;
	rla->n = n;
	memcpy(rla->ids, ids, n * sizeof ids[0]);

	cur = window_count_panes(w);
	if (cur != n && allow_spawn) {
		/*
		 * Bump mirroring so the split-window calls below create proxy
		 * placeholders without telling the remote to create windows.
		 * The trailing callback drops it again.
		 */
		rh->mirroring++;
		rla->dec_mirroring = 1;

		/* Kill local panes whose remote id is gone from the layout. */
		removed = 0;
		TAILQ_FOREACH_SAFE(wp, &w->panes, entry, wp1) {
			found = 0;
			for (i = 0; i < n; i++) {
				if (wp->remote_pane == ids[i]) {
					found = 1;
					break;
				}
			}
			if (!found) {
				xasprintf(&cmd, "kill-pane -t %%%u", wp->id);
				remote_queue_command(cmd);
				free(cmd);
				removed++;
			}
		}

		/* Split in placeholders until the count matches. */
		kept = cur - removed;
		for (i = kept; i < n; i++) {
			xasprintf(&cmd, "split-window -d -t @%u "
			    "'exec cat > /dev/null'", w->id);
			remote_queue_command(cmd);
			free(cmd);
		}
	}

	/* Replicate the exact geometry. */
	xasprintf(&cmd, "select-layout -t @%u '%s'", w->id, layout);
	remote_queue_command(cmd);
	free(cmd);

	/* Remap local panes to remote ids once the above have run. */
	item = cmdq_get_callback(remote_apply_layout_cb, rla);
	cmdq_append(NULL, item);
}

/*
 * Trailing callback for remote_apply_layout: walk the window's panes in
 * order and assign each the remote pane id of the matching layout cell.
 */
static enum cmd_retval
remote_apply_layout_cb(__unused struct cmdq_item *item, void *data)
{
	struct remote_layout_apply	*rla = data;
	struct window			*w, *wsearch;
	struct window_pane		*wp;
	u_int				 i = 0;

	w = NULL;
	RB_FOREACH(wsearch, windows, &windows) {
		if (wsearch->id == rla->local_window) {
			w = wsearch;
			break;
		}
	}
	if (w != NULL) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (i >= rla->n)
				break;
			wp->flags |= PANE_REMOTE;
			wp->remote = rla->rh;
			wp->remote_pane = rla->ids[i];
			log_debug("remote: mapped %%%u -> remote %%%u",
			    wp->id, rla->ids[i]);
			i++;
		}
	}

	if (rla->dec_mirroring && rla->rh->mirroring > 0)
		rla->rh->mirroring--;
	free(rla);

	return (CMD_RETURN_NORMAL);
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
	struct remote_conn	*pc, *loop;
	char			*sname, *cmd;
	u_int			 ids[256], npanes, i;
	int			 first_win;

	/*
	 * If the primary attached with no specific target, it landed on the
	 * remote's most-recent session. Pin it to the first discovered session
	 * (deterministic) so we know which session it streams and don't open a
	 * duplicate streaming connection for it.
	 */
	pc = NULL;
	TAILQ_FOREACH(loop, &rh->conns, entry) {
		if (loop->primary) {
			pc = loop;
			break;
		}
	}
	if (pc != NULL && pc->session == NULL) {
		rs = TAILQ_FIRST(&rh->sessions);
		if (rs != NULL) {
			pc->session = xstrdup(rs->name);
			if (pc->job != NULL) {
				struct bufferevent *bev = job_get_event(pc->job);
				if (bev != NULL) {
					xasprintf(&cmd,
					    "switch-client -t '%s'\n", rs->name);
					bufferevent_write(bev, cmd, strlen(cmd));
					free(cmd);
				}
			}
		}
	}

	TAILQ_FOREACH(rs, &rh->sessions, entry) {
		xasprintf(&sname, "%s/%s", rh->name, rs->name);

		if (session_find(sname) != NULL) {
			free(sname);
			continue;
		}

		first_win = 1;
		TAILQ_FOREACH(rw, &rs->windows, entry) {
			/*
			 * Number of panes the window needs, taken from the
			 * remote layout (authoritative); fall back to one.
			 * The session is not yet marked remote, so these
			 * commands create plain placeholder panes without
			 * touching the remote.
			 */
			npanes = remote_layout_pane_ids(rw->layout, ids,
			    nitems(ids));
			if (npanes == 0)
				npanes = 1;

			if (first_win) {
				xasprintf(&cmd,
				    "new-session -d -s '%s' -n '%s' "
				    "-x 80 -y 24 'exec cat > /dev/null'",
				    sname, rw->name);
				first_win = 0;
			} else {
				/* No -d: select it so the splits below hit it. */
				xasprintf(&cmd,
				    "new-window -t '%s:' -n '%s' "
				    "'exec cat > /dev/null'",
				    sname, rw->name);
			}
			remote_queue_command(cmd);
			free(cmd);

			/* Split in the remaining panes for this window. */
			for (i = 1; i < npanes; i++) {
				xasprintf(&cmd,
				    "split-window -t '%s' "
				    "'exec cat > /dev/null'", sname);
				remote_queue_command(cmd);
				free(cmd);
			}
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
		remote_queue_command(cmd);
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
		 * Match remote windows to local windows in order. Mark the
		 * window and all its panes as remote (mapping pane ids comes
		 * from applying the layout below), then replicate the remote
		 * geometry and refine the pane->remote-id mapping.
		 */
		wl = RB_MIN(winlinks, &s->windows);
		TAILQ_FOREACH(rw, &rs->windows, entry) {
			if (wl == NULL)
				break;

			wl->window->remote = rh;
			wl->window->remote_window = rw->id;
			wl->window->remote_sx = wl->window->sx;
			wl->window->remote_sy = wl->window->sy;

			TAILQ_FOREACH(wp, &wl->window->panes, entry) {
				wp->flags |= PANE_REMOTE;
				wp->remote = rh;
				wp->remote_pane = UINT_MAX;
			}

			remote_apply_layout(rh, rw->id, rw->layout, 0);

			wl = RB_NEXT(winlinks, &s->windows, wl);
		}
	}

	/*
	 * Request a refresh so any pending %output from the remote
	 * panes gets delivered. Don't use send-keys as it injects
	 * spurious prompts.
	 */
	{
		struct bufferevent *bev = remote_primary_bev(rh);
		if (bev != NULL) {
			bufferevent_write(bev, "refresh-client\n",
			    strlen("refresh-client\n"));
		}
	}

	/*
	 * The tree and proxy panes are now in place; bring up a streaming
	 * connection for every remote session so all of them deliver output.
	 */
	remote_spawn_session_conns(rh);

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
	struct remote_conn	*rc;
	struct bufferevent	*bev;
	char			 cmd[256];

	if (rh->state != REMOTE_READY)
		return;

	/* Prefer the connection attached to that session; else the primary. */
	rc = remote_find_conn(rh, remote_session);
	if (rc != NULL && rc->state == REMOTE_READY && rc->job != NULL)
		bev = job_get_event(rc->job);
	else
		bev = remote_primary_bev(rh);
	if (bev == NULL)
		return;

	snprintf(cmd, sizeof cmd, "new-window -t '%s'\n", remote_session);
	bufferevent_write(bev, cmd, strlen(cmd));
	log_debug("remote: created window on %s/%s", rh->name, remote_session);
}

/*
 * A local proxy window changed size. Tell the remote control client to
 * adopt that size; the remote then relayouts and sends back a
 * %layout-change which we replicate, keeping panes 1:1. Deduped on the
 * last size sent to avoid oscillation.
 */
void
remote_window_resize(struct window *w, u_int sx, u_int sy)
{
	struct remote_host	*rh = w->remote;
	struct remote_conn	*rc;
	struct session		*s, *sloop;
	struct bufferevent	*bev = NULL;
	char			 cmd[64];

	if (rh == NULL || rh->state != REMOTE_READY)
		return;
	if (sx == 0 || sy == 0)
		return;
	if (w->remote_sx == sx && w->remote_sy == sy)
		return;
	w->remote_sx = sx;
	w->remote_sy = sy;

	/*
	 * refresh-client -C sets the *sending* client's size, so it must go
	 * out on the connection attached to this window's session. Find that
	 * session, then its connection.
	 */
	s = NULL;
	RB_FOREACH(sloop, sessions, &sessions) {
		if (sloop->remote == rh &&
		    winlink_find_by_window(&sloop->windows, w) != NULL) {
			s = sloop;
			break;
		}
	}
	if (s != NULL && s->remote_session != NULL) {
		rc = remote_find_conn(rh, s->remote_session);
		if (rc != NULL && rc->state == REMOTE_READY && rc->job != NULL)
			bev = job_get_event(rc->job);
	}
	if (bev == NULL)
		return;

	snprintf(cmd, sizeof cmd, "refresh-client -C %ux%u\n", sx, sy);
	bufferevent_write(bev, cmd, strlen(cmd));
	log_debug("remote: resize %s @%u -> %ux%u", rh->name,
	    w->remote_window, sx, sy);
}

/*
 * Send raw bytes to a remote pane as a hex send-keys command. This bypasses
 * the remote tmux's own key handling and is used for content that is already
 * an exact terminal byte sequence: UTF-8 text and mouse reports.
 */
static void
remote_send_bytes(struct remote_host *rh, u_int pane, const u_char *buf,
    size_t len)
{
	struct bufferevent	*bev;
	char			*cmd, *p;
	size_t			 i, size, off;

	if (len == 0)
		return;
	bev = remote_primary_bev(rh);
	if (bev == NULL)
		return;

	size = 32 + len * 3 + 2;
	cmd = xmalloc(size);
	off = xsnprintf(cmd, size, "send-keys -t %%%u -H", pane);
	p = cmd + off;
	for (i = 0; i < len; i++)
		p += xsnprintf(p, size - (p - cmd), " %02x", buf[i]);
	*p++ = '\n';
	*p = '\0';

	bufferevent_write(bev, cmd, strlen(cmd));
	free(cmd);
}

/*
 * Send a keystroke or mouse event to a remote pane via the control mode
 * connection. Plain text (ASCII and UTF-8) and mouse events are sent as raw
 * bytes; other keys (control, modified, function) are sent by name so the
 * remote tmux encodes them according to the remote application's modes.
 */
void
remote_send_key(struct window_pane *wp, key_code key, struct mouse_event *m)
{
	struct remote_host	*rh = wp->remote;
	struct bufferevent	*bev;
	struct screen		*s = wp->screen;
	struct utf8_data	 ud;
	const char		*keystr, *buf;
	char			 cmd[512];
	size_t			 len;
	u_int			 x, y;
	u_char			 ch;

	if (rh == NULL || rh->state != REMOTE_READY)
		return;
	if (wp->remote_pane == UINT_MAX)
		return; /* not mapped yet */

	bev = remote_primary_bev(rh);
	if (bev == NULL)
		return;

	/*
	 * Mouse: encode with the local mirrored screen mode (which tracks the
	 * remote application's mouse mode via %output) and send raw.
	 */
	if (KEYC_IS_MOUSE(key)) {
		if (m == NULL || m->ignore)
			return;
		if ((s->mode & ALL_MOUSE_MODES) == 0)
			return;
		if (cmd_mouse_at(wp, m, &x, &y, 0) != 0)
			return;
		if (!input_key_get_mouse(s, m, x, y, &buf, &len))
			return;
		remote_send_bytes(rh, wp->remote_pane, (const u_char *)buf, len);
		return;
	}

	/* Plain key (no modifiers): send its literal bytes. */
	if (!(key & ~KEYC_MASK_KEY)) {
		if (key >= 0x20 && key <= 0x7f) {
			ch = (u_char)key;
			remote_send_bytes(rh, wp->remote_pane, &ch, 1);
			return;
		}
		if (KEYC_IS_UNICODE(key)) {
			utf8_to_data(key, &ud);
			remote_send_bytes(rh, wp->remote_pane, ud.data, ud.size);
			return;
		}
	}

	/* Control, modified, and function keys: send by name. */
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
