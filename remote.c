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

/* Line buffer for control mode parsing. */
static void	remote_update_cb(struct job *);
static void	remote_complete_cb(struct job *);
static void	remote_free_cb(void *);
static void	remote_parse_line(struct remote_host *, const char *);
static void	remote_parse_sessions(struct remote_host *, const char *);
static void	remote_parse_windows(struct remote_host *, const char *);
static void	remote_parse_panes(struct remote_host *, const char *);

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

void
remote_connect(struct remote_host *rh)
{
	char	*cmd;

	if (rh->state == REMOTE_CONNECTING || rh->state == REMOTE_READY)
		return;

	free(rh->error);
	rh->error = NULL;
	rh->state = REMOTE_CONNECTING;

	/*
	 * Build the ssh command. We use tmux -C (control mode) to get a
	 * text-protocol interface to the remote tmux server.
	 */
	if (rh->tmux_target != NULL)
		xasprintf(&cmd, "ssh %s tmux -C new-session -A -t %s",
		    rh->ssh_target, rh->tmux_target);
	else
		xasprintf(&cmd, "ssh %s tmux -C new-session -A",
		    rh->ssh_target);

	rh->job = job_run(cmd, 0, NULL, NULL, NULL, NULL,
	    remote_update_cb, remote_complete_cb, remote_free_cb,
	    rh, JOB_NOWAIT | JOB_KEEPWRITE, -1, -1);
	free(cmd);
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
	 */
	bev = job_get_event(rh->job);
	if (bev == NULL)
		return;
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
 *
 * For V1 we use a simple state machine: after connection we send
 * list-sessions/list-windows/list-panes and parse their output blocks.
 */

/* Parsing state for multi-line command responses. */
enum remote_parse_state {
	PARSE_IDLE,
	PARSE_SESSIONS,
	PARSE_WINDOWS,
	PARSE_PANES
};

static enum remote_parse_state parse_state = PARSE_IDLE;

static void
remote_parse_line(struct remote_host *rh, const char *line)
{
	/* Handle control mode greeting / ready state. */
	if (strncmp(line, "%begin ", 7) == 0) {
		/* A command response is starting. */
		return;
	}
	if (strncmp(line, "%end ", 5) == 0) {
		/* Command response ended; advance state. */
		switch (parse_state) {
		case PARSE_IDLE:
			break;
		case PARSE_SESSIONS:
			parse_state = PARSE_WINDOWS;
			break;
		case PARSE_WINDOWS:
			parse_state = PARSE_PANES;
			break;
		case PARSE_PANES:
			parse_state = PARSE_IDLE;
			break;
		}
		return;
	}
	if (strncmp(line, "%error ", 7) == 0) {
		parse_state = PARSE_IDLE;
		return;
	}
	if (strncmp(line, "%exit", 5) == 0) {
		rh->state = REMOTE_DISCONNECTED;
		return;
	}

	/* On first output, mark as ready and trigger refresh. */
	if (rh->state == REMOTE_CONNECTING) {
		rh->state = REMOTE_READY;
		remote_clear_tree(rh);
		parse_state = PARSE_SESSIONS;
		remote_refresh(rh);
		return;
	}

	/* Parse data lines based on current state. */
	switch (parse_state) {
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

	/* Find the session. */
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

	/* Find the window across all sessions. */
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

void
remote_open(struct remote_host *rh, const char *target, struct cmdq_item *item)
{
	char	*cmd;

	/*
	 * V1: Open a new window with an SSH session to the remote.
	 * Future versions will use a more integrated proxy approach.
	 */
	if (target != NULL)
		xasprintf(&cmd, "ssh %s -t tmux attach-session -t %s",
		    rh->ssh_target, target);
	else
		xasprintf(&cmd, "ssh %s -t tmux attach-session",
		    rh->ssh_target);

	/* Use cmdq_print to communicate with the user for now. */
	cmdq_print(item, "Opening remote: %s", cmd);

	/*
	 * Spawn a new window with the ssh command. We create a simple
	 * spawn context to open the remote session.
	 */
	{
		struct client		*tc;
		struct session		*s;
		struct spawn_context	 sc;
		struct winlink		*new_wl;
		char			*cause = NULL;
		char			*wname;

		tc = cmdq_get_target_client(item);
		if (tc == NULL || tc->session == NULL) {
			cmdq_error(item, "no current session");
			free(cmd);
			return;
		}
		s = tc->session;

		memset(&sc, 0, sizeof sc);
		sc.item = item;
		sc.s = s;
		sc.tc = tc;
		sc.argc = 3;
		sc.argv = xcalloc(3, sizeof *sc.argv);
		sc.argv[0] = xstrdup("ssh");
		sc.argv[1] = xstrdup(rh->ssh_target);
		sc.argv[2] = xstrdup("-t");

		/* Build full argv for ssh -t tmux attach */
		cmd_free_argv(sc.argc, sc.argv);
		if (target != NULL) {
			sc.argc = 7;
			sc.argv = xcalloc(7, sizeof *sc.argv);
			sc.argv[0] = xstrdup("ssh");
			sc.argv[1] = xstrdup(rh->ssh_target);
			sc.argv[2] = xstrdup("-t");
			sc.argv[3] = xstrdup("tmux");
			sc.argv[4] = xstrdup("attach-session");
			sc.argv[5] = xstrdup("-t");
			sc.argv[6] = xstrdup(target);
		} else {
			sc.argc = 5;
			sc.argv = xcalloc(5, sizeof *sc.argv);
			sc.argv[0] = xstrdup("ssh");
			sc.argv[1] = xstrdup(rh->ssh_target);
			sc.argv[2] = xstrdup("-t");
			sc.argv[3] = xstrdup("tmux");
			sc.argv[4] = xstrdup("attach-session");
		}
		sc.environ = environ_create();
		xasprintf(&wname, "remote:%s", rh->name);
		sc.name = wname;
		sc.idx = -1;
		sc.cwd = NULL;
		sc.flags = 0;

		new_wl = spawn_window(&sc, &cause);
		if (new_wl == NULL) {
			cmdq_error(item, "spawn failed: %s", cause);
			free(cause);
		}

		cmd_free_argv(sc.argc, sc.argv);
		environ_free(sc.environ);
		free(wname);
	}
	free(cmd);
}
