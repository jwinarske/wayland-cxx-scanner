# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-07-16

First stable release. The code generator and the header-only `wl/` framework are
committed to Semantic Versioning from this point.

### Added

#### Code generator

- Generates type-safe C++ bindings from a Wayland XML protocol, replacing the
  handwritten C bindings from `wayland-scanner`.
- Three output modes: `--mode=client-header`, `server-header`, and `c-header`
  (a C-compatible header) from a single XML source.
- `--std` selects the target standard: C++17, C++20, or C++23.
- `--emit-interface-tables` makes an extension protocol's header self-contained,
  carrying its own `wl_interface` tables and inline `wl_iface()`.
- CRTP proxy and resource classes with compile-time event/request dispatch — no
  `void*` casts in user code, and dispatch cost identical to raw C
  `wayland-scanner` output.

#### Header-only framework (`wl/`)

- Installs to `${includedir}/wl/` with no link-time dependencies of its own; a
  C++17 floor that also compiles clean at C++20 and C++23.
- Core client helpers: `registry`, `display`, `proxy`/`wl_ptr` ownership,
  `client_helpers`, `raii`, `fd_handle`, `event_map`, `span`.
- Input: `seat` (keyboard + pointer + touch via SeatManager), `keyboard`,
  `pointer`, `touch`, `cursor` (theme-following cursor manager).
- Scaling: `scale_policy` for buffer/viewport sizing under fractional scale.
- Protocol support headers: `xdg_shell`, `xdg_decoration`, `linux_dmabuf`,
  `dmabuf_feedback`, `presentation`/`present_feedback`, `data_device`,
  `ext_data_control`, `agl_shell`, `simple_shell`.
- Client-side decoration: `csd_plugin` interface, header-only `csd_fallback`,
  and `csd_common` color helpers.

#### Client-side decoration (examples)

- `DecoratedWindow` — a window frame that puts the decoration on a
  `wl_subsurface`, so an EGL- or Vulkan-rendered client can be decorated. It
  owns the xdg-decoration negotiation, the pointer gestures (move, resize,
  maximize, minimize), and the window-geometry/configure inverse.
- Plugins selected at build time by the `csd` option:
  `none` (default, nothing built), `ssd` (ask the compositor), `auto` (prefer
  the compositor, fall back to a plugin), or a named plugin — `gtk` (themed
  through GTK's own widgets), `cairo`, or `fallback`.
- Follows the desktop live: theme, decoration layout, double-click time, drag
  threshold, cursor theme and size all come from the toolkit, not from
  hardcoded values.
- Fractional-scale aware: the decoration surface carries its own `wp_viewport`,
  with a subpixel-seam guard at fractional scales.
- `DecoratedFrame` helper wraps the frame for an example to adopt CSD in a few
  lines; adopted by `simple-egl`, `xdg-csd`, and the Skia and Vulkan/dma-buf
  render demos.

#### Examples

- SHM, EGL/GLES, and Vulkan clients; `subsurfaces`, `presentation-shm`,
  `xdg-csd`/`xdg-ssd` decoration demos.
- Skia canvases (SHM, EGL, Vulkan/dma-buf, Skottie) behind a `skia_examples`
  option.
- Vulkan/dma-buf with explicit sync (`wp_linux_drm_syncobj`), fractional scale,
  and a shared `vk-common` bridge.
- Input and data: `key-input`, `text-input`, `clipboard`, `ext-data-control`.
- Shell integrations: `agl-presentation-*`, `ivi-presentation-shm`,
  `simple-shell-egl`; plus `imgui`, `shadertoy`, and `wayland-info`.

#### Build and CI

- Dual build systems — Meson and CMake — kept in lock-step, with a sync gate
  that checks their scanner invocations and compile defines agree.
- CI matrix across gcc and clang at C++17/20/23, a Fedora leg, an aarch64
  cross-compile, AddressSanitizer + UndefinedBehaviorSanitizer, CodeQL, and
  `clang-format`/`clang-tidy` enforcement.
