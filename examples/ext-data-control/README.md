<!-- SPDX-License-Identifier: MIT -->
# ext-data-control example

A **focus-free clipboard CLI** — copy and paste from the shell, with no window
and no keyboard focus — over the staging
[`ext-data-control-v1`](https://wayland.app/protocols/ext-data-control-v1)
protocol. It is the headless counterpart of the windowed
[`clipboard`](../clipboard) example.

## Why a second clipboard example

The core `wl_data_device` selection is delivered only to the client that holds
**keyboard focus**, and `set_selection` must carry a real input serial — so the
core clipboard has to be a window (that is what [`clipboard`](../clipboard) is).
`ext-data-control` removes both requirements: any client may read or set the
selection at any time. That is exactly what tools like `wl-copy` / `wl-paste`
use, and what makes a plain CLI possible.

The interesting part is that **the same helper drives both**. The framework's
`wl::DataDevice<App, Protocol>` is parameterized on a protocol-traits bundle;
the core clipboard uses `wl::CoreDataProtocol` and this example uses
`wl::ExtDataControlProtocol` (from `<wl/ext_data_control.hpp>`). Only two shape
flags differ — `set_selection` carries no serial, and the device is torn down by
its own `destroy` request rather than a version-gated `release` — so no clipboard
logic is duplicated.

## Usage

```
ext-data-control --paste [--primary] [--mime TYPE] [--list]
ext-data-control --copy  [--primary] [--mime TYPE] [--] [TEXT ...]
```

| Invocation | Effect |
|---|---|
| `--paste` | write the current selection to stdout |
| `--paste --list` | print the offered MIME types, one per line |
| `--paste --mime T` | receive exactly type `T` (error if not offered) |
| `--copy TEXT ...` | take the selection; data is the TEXT args joined with a space |
| `--copy` (no TEXT) | take the selection; data is read from stdin |
| `--copy --mime T` | offer exactly type `T` (e.g. a binary flavor) instead of the text set |
| `--primary` | act on the **primary** (middle-click) selection instead of the clipboard |

ext-data-control carries the primary selection on the same device as the
regular one, so `--primary` composes with everything above — `--paste
--primary`, `--copy --primary "text"`, `--paste --primary --list`. The two
selections are independent:

```sh
printf REG | ext-data-control --copy &            # regular clipboard
printf PRI | ext-data-control --copy --primary &  # primary selection
ext-data-control --paste            # -> REG
ext-data-control --paste --primary  # -> PRI
```

With no `--mime`, paste picks the first available of
`text/plain;charset=utf-8`, `text/plain`, `UTF8_STRING`, `STRING`, `TEXT`, and
copy offers all of them.

`--copy` stays in the **foreground**, serving the data on demand, until another
client takes the selection or it is interrupted (Ctrl-C) — it does not
daemonize. Redirect and background it yourself if you want that:

```sh
ext-data-control --copy "hello" &      # serve until superseded
ext-data-control --paste               # -> hello
printf '%s' "$PWD" | ext-data-control --copy   # copy from a pipe
```

It interoperates with any other ext-data-control client, including
`wl-copy`/`wl-paste`.

## Requirements

A compositor that implements `ext_data_control_manager_v1` — wlroots-based
compositors (Sway, river, …) with wlroots ≥ 0.18, recent KWin, and recent
Mutter. On a compositor that only offers the older `wlr-data-control` (or no
data-control at all) the tool prints a clear message and exits non-zero.

Build requirements: `wayland-client`, `wayland-protocols` (with the staging
`ext-data-control` protocol), `wayland-scanner`. No EGL, GPU, or `xkbcommon` —
the protocol is focus-free, so this CLI binds no keyboard.
