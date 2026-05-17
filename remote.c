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

#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "remote.h"

struct remote_hosts remote_hosts = TAILQ_HEAD_INITIALIZER(remote_hosts);

static void	remote_update_cb(struct job *);
static void	remote_complete_cb(struct job *);
static void	remote_free_cb(void *);
static void	remote_parse_line(struct remote_host *, const char *);
static void	remote_parse_sessions(struct remote_host *, const char *);
static void	remote_parse_windows(struct remote_host *, const char *);
static void	remote_parse_panes(struct remote_host *, const char *);
static void	remote_spawn_sessions(struct remote_host *);
static void	remote_destroy_sessions(struct remote_host *);

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

/*
 * Return the SSH ControlPath for this remote host.
 * Caller must free the result.
 */
char *
remote_control_path(struct remote_host *rh)
{
	char	*path;

	xasprintf(&path, "/tmp/tmux-remote-%s", rh->name);
	return (path);
}

/*
 * Connect to a remote host. This spawns an interactive window so the user
 * can authenticate (enter passphrase, etc). The window runs a script that:
 *   1. Establishes an SSH ControlMaster connection
 *   2. Calls "tmux remote-refresh <name>" to start the control-mode link
 *   3. Closes itself
 */
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
		/* No client to open a window in; try non-interactive. */
		remote_connect_control(rh);
		return;
	}
	s = tc->session;

	ctrl_path = remote_control_path(rh);

	/*
	 * Build a shell command that:
	 * - Establishes SSH ControlMaster (interactive for passphrase)
	 * - On success, triggers the background control-mode connection
	 * - Then exits (closing the auth window)
	 */
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
 * Start the background control-mode connection, optionally reusing an
 * SSH ControlMaster socket if one exists.
 */
void
remote_connect_control(struct remote_host *rh)
{
	char	*cmd, *ctrl_path;

	if (rh->state == REMOTE_READY)
		return;

	/* If we were CONNECTING from auth window, keep that state. */
	if (rh->state != REMOTE_CONNECTING) {
		free(rh->error);
		rh->error = NULL;
		rh->state = REMOTE_CONNECTING;
	}

	ctrl_path = remote_control_path(rh);

	/*
	 * Build the ssh command with ControlPath so it reuses the
	 * authenticated master connection.
	 */
	if (rh->tmux_target != NULL)
		xasprintf(&cmd,
		    "ssh -o 'ControlPath=%s' -o ControlMaster=auto "
		    "%s tmux -C new-session -A -t %s",
		    ctrl_path, rh->ssh_target, rh->tmux_target);
	else
		xasprintf(&cmd,
		    "ssh -o 'ControlPath=%s' -o ControlMaster=auto "
		    "%s tmux -C new-session -A",
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

	/*
	 * Send commands to the remote tmux control mode to list sessions,
	 * windows, and panes. The responses will be parsed asynchronously.
	 * After parsing completes, remote_spawn_sessions() creates local
	 * sessions for each remote session.
	 */
	bev = job_get_event(rh->job);
	if (bev == NULL)
		return;

	/* Clear existing tree before re-populating. */
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
	/* Nothing to free; rh owns everything. */
}

/*
 * Parse a line from the remote tmux control mode output.
 * Control mode outputs lines like:
 *   %begin <time> <num> <flags>
 *   <data lines>
 *   %end <time> <num> <flags>
 * Plus notifications like:
 *   %session-changed ...
 *   %exit
 */

static void
remote_parse_line(struct remote_host *rh, const char *line)
{
	if (strncmp(line, "%begin ", 7) == 0)
		return;

	if (strncmp(line, "%end ", 5) == 0) {
		/*
		 * First %end while CONNECTING means the attach succeeded.
		 * Mark ready and send list commands.
		 */
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
		rh->state = REMOTE_DISCONNECTED;
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
 * Create local tmux sessions mirroring remote sessions.
 *
 * For remote host "as3" with session "main" containing windows "editor"
 * and "logs", this creates local session "as3/main" with two windows,
 * each running a plain "ssh -t as3" (no inner tmux). The user navigates
 * windows and splits with their normal outer tmux keybindings.
 */
static void
remote_spawn_sessions(struct remote_host *rh)
{
	struct remote_session	*rs;
	char			*sname, *cmd, *error, *ctrl_path;
	char			*ssh_cmd;
	struct cmdq_state	*state;
	enum cmd_parse_status	 status;

	ctrl_path = remote_control_path(rh);

	/* Build the base SSH command that reuses the ControlMaster socket. */
	xasprintf(&ssh_cmd,
	    "ssh -o 'ControlPath=%s' -o ControlMaster=auto -t %s",
	    ctrl_path, rh->ssh_target);

	/*
	 * Build the SSH+tmux attach command. Each window attaches to the
	 * same remote session — the remote tmux handles windows/panes,
	 * the local tmux handles session switching via prefix+w.
	 */

	TAILQ_FOREACH(rs, &rh->sessions, entry) {
		xasprintf(&sname, "%s/%s", rh->name, rs->name);

		/* Skip if this local session already exists. */
		if (session_find(sname) != NULL) {
			free(sname);
			continue;
		}

		/*
		 * Create one local session per remote session. The pane
		 * runs ssh -t <host> tmux attach -t <session>, so the
		 * remote tmux handles windows/panes/persistence, and the
		 * local tmux handles session switching.
		 */
		xasprintf(&cmd,
		    "new-session -d -s '%s' "
		    "'%s tmux attach-session -t \"%s\"'",
		    sname, ssh_cmd, rs->name);

		state = cmdq_new_state(NULL, NULL, 0);
		status = cmd_parse_and_append(cmd, NULL, NULL,
		    state, &error);
		if (status == CMD_PARSE_ERROR) {
			log_debug("remote: %s: %s", sname, error);
			free(error);
		}
		cmdq_free_state(state);
		free(cmd);

		/*
		 * Set session options:
		 * - remain-on-exit: panes stay when SSH disconnects
		 * - detach-on-destroy: switch to another session, don't detach
		 * - default-command: prefix+c opens SSH to remote, not local shell
		 */
		xasprintf(&cmd,
		    "set-option -t '%s' remain-on-exit on \\; "
		    "set-option -t '%s' detach-on-destroy no-detached \\; "
		    "set-option -t '%s' default-command "
		    "'%s tmux attach-session -t \"%s\"'",
		    sname, sname, sname, ssh_cmd, rs->name);
		state = cmdq_new_state(NULL, NULL, 0);
		cmd_parse_and_append(cmd, NULL, NULL, state, &error);
		cmdq_free_state(state);
		free(cmd);

		log_debug("remote: created session %s", sname);
		free(sname);
	}
	free(ssh_cmd);
	free(ctrl_path);
}

/*
 * Destroy all local sessions that belong to this remote host.
 * Sessions are identified by the "hostname/" prefix in their name.
 */
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
 * Open/switch to a remote session. If target is NULL, switch to the first
 * session for this host. If target names a remote session, switch to the
 * corresponding local session "host/target".
 */
void
remote_open(struct remote_host *rh, const char *target, struct cmdq_item *item)
{
	struct session	*s;
	struct client	*tc;
	char		*sname;

	if (target != NULL)
		xasprintf(&sname, "%s/%s", rh->name, target);
	else {
		/* Pick first session for this host. */
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

	/* Switch this client to the remote session. */
	server_client_set_session(tc, s);
	free(sname);
}
