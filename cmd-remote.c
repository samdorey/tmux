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

/*
 * Remote host management commands:
 *   remote-add <name> <ssh-target> [tmux-target]
 *   remote-remove <name>
 *   remote-list
 *   remote-refresh [name]
 *   remote-open <name>:<session>[:window[.pane]]
 */

static enum cmd_retval	cmd_remote_add_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_remote_remove_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_remote_list_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_remote_refresh_exec(struct cmd *,
			    struct cmdq_item *);
static enum cmd_retval	cmd_remote_open_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_remote_add_entry = {
	.name = "remote-add",
	.alias = "remotea",

	.args = { "", 1, 3, NULL },
	.usage = "name [ssh-target] [tmux-target]",

	.target = { .flags = CMD_FIND_SESSION },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_remote_add_exec
};

const struct cmd_entry cmd_remote_remove_entry = {
	.name = "remote-remove",
	.alias = "remoterm",

	.args = { "", 1, 1, NULL },
	.usage = "name",

	.flags = CMD_AFTERHOOK,
	.exec = cmd_remote_remove_exec
};

const struct cmd_entry cmd_remote_list_entry = {
	.name = "remote-list",
	.alias = "remotels",

	.args = { "", 0, 0, NULL },
	.usage = "",

	.flags = CMD_AFTERHOOK|CMD_READONLY,
	.exec = cmd_remote_list_exec
};

const struct cmd_entry cmd_remote_refresh_entry = {
	.name = "remote-refresh",
	.alias = "remoter",

	.args = { "", 0, 1, NULL },
	.usage = "[name]",

	.flags = CMD_AFTERHOOK,
	.exec = cmd_remote_refresh_exec
};

const struct cmd_entry cmd_remote_open_entry = {
	.name = "remote-open",
	.alias = "remoteo",

	.args = { "", 1, 1, NULL },
	.usage = "name[:session[:window[.pane]]]",

	.target = { .flags = CMD_FIND_SESSION },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_remote_open_exec
};

static enum cmd_retval
cmd_remote_add_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	const char		*name, *ssh_target, *tmux_target;
	struct remote_host	*rh;

	name = args_string(args, 0);
	ssh_target = args_string(args, 1);
	tmux_target = args_string(args, 2); /* NULL if not provided */

	/* If only one arg, use it as both name and ssh target. */
	if (ssh_target == NULL)
		ssh_target = name;

	if (remote_find(name) != NULL) {
		cmdq_error(item, "remote already exists: %s", name);
		return (CMD_RETURN_ERROR);
	}

	rh = remote_add(name, ssh_target, tmux_target);
	remote_connect(rh, item);

	cmdq_print(item, "Added remote: %s (%s)", name, ssh_target);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_remote_remove_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	const char		*name;
	struct remote_host	*rh;

	name = args_string(args, 0);

	rh = remote_find(name);
	if (rh == NULL) {
		cmdq_error(item, "remote not found: %s", name);
		return (CMD_RETURN_ERROR);
	}

	remote_remove(rh);
	cmdq_print(item, "Removed remote: %s", name);
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_remote_list_exec(__unused struct cmd *self, struct cmdq_item *item)
{
	struct remote_host	*rh;
	const char		*state;

	TAILQ_FOREACH(rh, &remote_hosts, entry) {
		switch (rh->state) {
		case REMOTE_DISCONNECTED:
			state = "disconnected";
			break;
		case REMOTE_CONNECTING:
			state = "connecting";
			break;
		case REMOTE_READY:
			state = "ready";
			break;
		case REMOTE_FAILED:
			state = "failed";
			break;
		default:
			state = "unknown";
			break;
		}
		if (rh->error != NULL)
			cmdq_print(item, "%s: %s [%s] (%s)",
			    rh->name, rh->ssh_target, state, rh->error);
		else
			cmdq_print(item, "%s: %s [%s]",
			    rh->name, rh->ssh_target, state);
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_remote_refresh_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	const char		*name;
	struct remote_host	*rh;

	name = args_string(args, 0);
	if (name != NULL) {
		rh = remote_find(name);
		if (rh == NULL) {
			cmdq_error(item, "remote not found: %s", name);
			return (CMD_RETURN_ERROR);
		}
		switch (rh->state) {
		case REMOTE_DISCONNECTED:
		case REMOTE_FAILED:
			remote_connect(rh, item);
			break;
		case REMOTE_CONNECTING:
			/* Called from auth window; start control mode. */
			remote_connect_control(rh);
			break;
		case REMOTE_READY:
			remote_refresh(rh);
			break;
		}
	} else {
		TAILQ_FOREACH(rh, &remote_hosts, entry) {
			switch (rh->state) {
			case REMOTE_DISCONNECTED:
			case REMOTE_FAILED:
				remote_connect(rh, item);
				break;
			case REMOTE_CONNECTING:
				remote_connect_control(rh);
				break;
			case REMOTE_READY:
				remote_refresh(rh);
				break;
			}
		}
	}
	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_remote_open_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	const char		*spec;
	char			*name, *colon, *target = NULL;
	struct remote_host	*rh;

	spec = args_string(args, 0);

	/* Parse name:target format. */
	name = xstrdup(spec);
	colon = strchr(name, ':');
	if (colon != NULL) {
		*colon = '\0';
		target = colon + 1;
		if (*target == '\0')
			target = NULL;
	}

	rh = remote_find(name);
	if (rh == NULL) {
		cmdq_error(item, "remote not found: %s", name);
		free(name);
		return (CMD_RETURN_ERROR);
	}

	remote_open(rh, target, item);
	free(name);
	return (CMD_RETURN_NORMAL);
}
