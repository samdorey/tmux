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

#ifndef REMOTE_H
#define REMOTE_H

#include "tmux.h"

/* Remote host connection states. */
enum remote_state {
	REMOTE_DISCONNECTED,
	REMOTE_CONNECTING,
	REMOTE_READY,
	REMOTE_FAILED
};

/* A cached pane from the remote tmux. */
struct remote_pane {
	u_int				 id;
	char				*title;
	int				 active;
	TAILQ_ENTRY(remote_pane)	 entry;
};
TAILQ_HEAD(remote_panes, remote_pane);

/* A cached window from the remote tmux. */
struct remote_window {
	u_int				 id;
	int				 idx;
	char				*name;
	int				 active;
	char				*layout;	/* #{window_layout} */
	struct remote_panes		 panes;
	TAILQ_ENTRY(remote_window)	 entry;
};
TAILQ_HEAD(remote_windows, remote_window);

/* A cached session from the remote tmux. */
struct remote_session {
	u_int				 id;
	char				*name;
	int				 attached;
	struct remote_windows		 windows;
	TAILQ_ENTRY(remote_session)	 entry;
};
TAILQ_HEAD(remote_sessions, remote_session);

/* Parsing state for control mode responses. */
enum remote_parse_state {
	PARSE_IDLE,
	PARSE_SESSIONS,
	PARSE_WINDOWS,
	PARSE_PANES
};

struct remote_host;

/*
 * A single control-mode connection. There is one per attached remote
 * session (control mode only streams %output for the session its client is
 * attached to), all multiplexed over the host's shared SSH ControlMaster.
 */
struct remote_conn {
	struct remote_host		*rh;
	char				*session;	/* remote session streamed */
	int				 primary;	/* drives tree discovery */

	struct job			*job;
	enum remote_state		 state;
	enum remote_parse_state		 parse_state;

	TAILQ_ENTRY(remote_conn)	 entry;
};
TAILQ_HEAD(remote_conns, remote_conn);

/* A remote host definition. */
struct remote_host {
	char				*name;
	char				*ssh_target;
	char				*tmux_target;	/* optional -t arg */

	enum remote_state		 state;		/* mirrors primary conn */
	char				*error;

	struct remote_conns		 conns;
	struct remote_sessions		 sessions;

	int				 mirroring;	/* suppress remote-side
							   window creation while
							   mirroring remote tree */
	u_int				 pending_window;/* local @id awaiting a
							   %window-add mapping,
							   or UINT_MAX */

	TAILQ_ENTRY(remote_host)	 entry;
};
TAILQ_HEAD(remote_hosts, remote_host);

/* Global list of remotes. */
extern struct remote_hosts remote_hosts;

/* remote.c */
void			 remote_init(void);
void			 remote_destroy(void);
struct remote_host	*remote_add(const char *, const char *, const char *);
void			 remote_remove(struct remote_host *);
struct remote_host	*remote_find(const char *);
void			 remote_connect(struct remote_host *,
			     struct cmdq_item *);
void			 remote_connect_control(struct remote_host *);
void			 remote_disconnect(struct remote_host *);
void			 remote_refresh(struct remote_host *);
void			 remote_clear_tree(struct remote_host *);
void			 remote_open(struct remote_host *, const char *,
			     struct cmdq_item *);
char			*remote_control_path(struct remote_host *);
void			 remote_send_key(struct window_pane *, key_code,
			     struct mouse_event *);
void			 remote_paste_input(struct window_pane *, const char *,
			     size_t);
void			 remote_create_window(struct remote_host *,
			     const char *, u_int);
void			 remote_split_window(struct remote_host *, u_int,
			     u_int, int);
struct window_pane	*remote_find_proxy_pane(struct remote_host *, u_int);
void			 remote_window_resize(struct window *, u_int, u_int);
void			 remote_apply_layout(struct remote_host *, u_int,
			     const char *, int);

#endif /* REMOTE_H */
