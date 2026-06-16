# tmux + remote session mounting

> A fork of [tmux](https://github.com/tmux/tmux) that lets you **mount tmux
> sessions from a remote host over SSH** and use them as if they were local —
> `prefix + w` to switch to them, split and resize their panes, scroll their
> history, all live.

## What it does

Normally, to use tmux on a remote machine you SSH in and attach to its tmux — a
tmux *inside* your local tmux, with its own prefix and a nested world. This fork
instead **mirrors** the remote sessions into your local tmux: each remote
session shows up as a local session named `host/session`, its windows and panes
rendered locally, its output streamed in real time over an SSH control‑mode
connection.

So `prefix + w` lists your local sessions *and* every remote one together. You
move between local and remote panes with the same keys — no nesting, no second
prefix.

## Quick start

Build it (it's a normal tmux build — needs a C compiler, `make`, `autoconf`,
`automake`, `pkg-config`, libevent and ncurses):

```sh
git clone -b remote-tree-mount-v1 https://github.com/samdorey/tmux.git
cd tmux
sh autogen.sh && ./configure && make
```

Run it and mount a host (the remote needs `tmux` and must be reachable over
SSH — ideally key-based, e.g. via `~/.ssh/config`):

```
$ ./tmux
# then, inside tmux:
:remote-add myhost user@host
```

The remote's sessions now appear locally as `myhost/<session>`. Press
`prefix + w` to see them in the tree and switch in.

## Commands

| command | what it does |
| --- | --- |
| `remote-add <name> <ssh-target> [session]` | mount a host's sessions as `<name>/<session>` |
| `remote-list` | show mounted hosts and their connection state |
| `remote-refresh [name]` | re-sync the session tree |
| `remote-open <name>[:session]` | switch to a mounted session |
| `remote-remove <name>` | unmount a host |

## Features

- **All sessions stream**, not just one — a control‑mode connection per remote
  session, multiplexed over a single shared SSH connection.
- **Multi-pane windows** mirrored with the remote's exact geometry.
- **Live everything** — output, keystrokes, mouse, and UTF‑8 input.
- **Size sync** — resize your terminal and the remote window follows; panes
  reflow 1:1.
- **Splits, new windows, and `Ctrl-D` close** propagate both directions —
  splitting the right pane in the right direction.
- Mounting an **empty/fresh remote** bootstraps a session for you.

## How it works

Each remote session is driven by a `tmux -C` (control mode) client over SSH.
Local "proxy" panes run a placeholder process; their on‑screen content is fed
from the remote's `%output`, and your keystrokes are sent back with
`send-keys`. The remote's `window_layout` is the source of truth for pane
geometry and identity, and `%layout-change` notifications keep everything in
sync as you split, resize, and close.

## Status

**v0.1.0** — working and in daily use; see
[Releases](https://github.com/samdorey/tmux/releases). Known rough edges: split
*ratios* snap to the remote's 50/50 default, and pane *titles* show the local
host (window names are correct). Built on tmux `next-3.7`; tested against a
Linux remote running tmux `3.5a`.

---

This is a fork of tmux. For tmux itself — full build dependencies, the manual,
and upstream documentation — see the original [`README`](README) and
<https://github.com/tmux/tmux>.

<!-- v0.1.0 -->
