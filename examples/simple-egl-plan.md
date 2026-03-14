# Plan: Add `simple-egl` Example

Creates `examples/simple-egl/` — a single-process client-only C++23 app that connects to a real Wayland compositor, presents an XDG toplevel window, sets up EGL + OpenGL ES 2, and runs a colour-cycling render loop.

---

## 1 · Protocol XML Strategy

Use **system-installed XMLs** — no in-repo copies needed. Locate them at meson configure time:

```meson
wayland_xml   = dependency('wayland-scanner').get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = dependency('wayland-protocols').get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
```

If either pkg-config lookup fails, `subdir_done()` skips the example silently.

---

## 2 · Protocol Scanner Invocations

Two `custom_target()` calls (client-header mode only — no server headers needed):

```meson
custom_target('simple-egl-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'])

custom_target('simple-egl-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'])
```

Generated namespaces: `wayland::client` and `xdg_shell::client`.

---

## 3 · File Structure

**New files:**
```
examples/simple-egl/
    meson.build          ← deps, custom_targets, executable()
    main.cpp             ← all application code (single TU)
```

**Modified files:**
```
examples/meson.build     ← add: subdir('simple-egl')
```

No `protocol.xml`, no `server.cpp`, no `roundtrip.cpp` — this is a single-process example connecting to the running desktop compositor.

---

## 4 · `main.cpp` Architecture

**Include order:**
1. EGL/GLES system headers (C linkage)
2. `wayland_client.hpp` + `xdg_shell_client.hpp` (generated)
3. `<wayland-client-protocol.h>` (for system `wl_*_interface` symbols)
4. `<wayland-egl.h>` (for `wl_egl_window`)
5. Framework headers: `<wl/registry.hpp>`, `<wl/wl_ptr.hpp>`
6. `wl_iface()` definitions (§5)
7. CRTP concrete handler structs
8. `App` class
9. `main()`

**`App` members:**

| Category | Member | Type |
|---|---|---|
| Wayland core | `display_` | `wl_display*` |
| Wayland core | `registry_` | `wl::CRegistry` |
| Wayland core | `compositor_proxy_` | `wl_proxy*` (raw — no events) |
| Wayland core | `surface_proxy_` | `wl_proxy*` (raw — no events) |
| CRTP handlers | `xdg_wm_base_` | `wl::WlPtr<XdgWmBaseHandler>` |
| CRTP handlers | `xdg_surface_` | `wl::WlPtr<XdgSurfaceHandler>` |
| CRTP handlers | `xdg_toplevel_` | `wl::WlPtr<XdgToplevelHandler>` |
| CRTP handlers | `seat_` | `wl::WlPtr<SeatHandler>` |
| CRTP handlers | `keyboard_` | `wl::WlPtr<KeyboardHandler>` |
| EGL | `egl_display_` | `EGLDisplay` (init `EGL_NO_DISPLAY`) |
| EGL | `egl_context_` | `EGLContext` (init `EGL_NO_CONTEXT`) |
| EGL | `egl_surface_` | `EGLSurface` (init `EGL_NO_SURFACE`) |
| EGL | `egl_config_` | `EGLConfig` |
| EGL | `egl_window_` | `wl_egl_window*` (init `nullptr`) |
| State | `running_` | `bool` (init `true`) |
| State | `configured_` | `bool` (init `false`) |
| State | `width_`, `height_` | `int` (init `800`, `600`) |
| State | `frame_count_` | `uint64_t` |
| Globals | `compositor_name_/ver_` | `uint32_t` |
| Globals | `xdg_wm_base_name_/ver_` | `uint32_t` |
| Globals | `seat_name_/ver_` | `uint32_t` |

**`App` lifecycle:**
```
App::Run()
  ├── ConnectDisplay()       — wl_display_connect(nullptr)
  ├── ScanGlobals()          — registry_.OnGlobal records name/ver per interface;
  │                            wl_display_roundtrip() to collect all globals
  ├── BindGlobals()          — raw wl_registry_bind for compositor;
  │                            registry_.Bind<xdg_wm_base_traits> for xdg_wm_base;
  │                            registry_.Bind<wl_seat_traits> for seat
  ├── CreateSurfaces()       — wl_proxy_marshal for wl_surface;
  │                            XdgWmBaseHandler::GetXdgSurface() → xdg_surface;
  │                            XdgSurfaceHandler::GetToplevel() → xdg_toplevel;
  │                            set title/app-id; wl_display_roundtrip() for configure
  ├── InitEgl()              — (§6)
  └── MainLoop()             — (§7)

~App()
  ├── CleanupEgl() — EGLSurface → EGLContext → wl_egl_window → eglTerminate
  ├── CRTP objects auto-destroyed by WlPtr (sends Destroy + wl_proxy_destroy)
  ├── wl_proxy_destroy(surface_proxy_), wl_proxy_destroy(compositor_proxy_)
  └── wl_display_disconnect(display_)
```

**CRTP handler structs:**

| Struct | Inherits | Events |
|---|---|---|
| `XdgWmBaseHandler` | `CXdgWmBase<XdgWmBaseHandler>` | `OnPing(serial)` → `Pong(serial)` |
| `XdgSurfaceHandler` | `CXdgSurface<XdgSurfaceHandler>` | `OnConfigure(serial)` → `AckConfigure(serial)`, set `configured_=true` |
| `XdgToplevelHandler` | `CXdgToplevel<XdgToplevelHandler>` | `OnConfigure(w,h,array)` → update `width_/height_`; `OnClose()` → `running_=false` |
| `SeatHandler` | `CWlSeat<SeatHandler>` | `OnCapabilities(caps)` → if keyboard capable, call `GetKeyboard()` |
| `KeyboardHandler` | `CWlKeyboard<KeyboardHandler>` | `OnKey(…)` → ESC pressed → `running_=false`; `OnKeymap(fmt,fd,size)` → `close(fd)` |

> **Key note:** For event-less interfaces (`wl_compositor`, `wl_surface`), store raw `wl_proxy*` — do **not** wrap in CRTP handlers, as the scanner won't emit a listener table for them.

Child object factory methods on handlers (using `_MarshalNew`):
- `XdgWmBaseHandler::GetXdgSurface(wl_proxy* surface)` → `xdg_surface`
- `XdgSurfaceHandler::GetToplevel()` → `xdg_toplevel`
- `SeatHandler::GetKeyboard()` → `wl_keyboard`

---

## 5 · `wl_iface()` Definitions

**Core Wayland interfaces** — forward to system pre-built symbols from `<wayland-client-protocol.h>`:

```cpp
// in namespace wayland::client:
const wl_interface& wl_compositor_traits::wl_iface() noexcept { return wl_compositor_interface; }
const wl_interface& wl_surface_traits::wl_iface()    noexcept { return wl_surface_interface; }
const wl_interface& wl_seat_traits::wl_iface()       noexcept { return wl_seat_interface; }
const wl_interface& wl_keyboard_traits::wl_iface()   noexcept { return wl_keyboard_interface; }
```

**XDG Shell interfaces** — no system pre-built symbol; define manually following the `examples/minimal/client.cpp` pattern. Build in dependency order: `xdg_toplevel` → `xdg_surface` → `xdg_wm_base`. Key format strings:

| Interface | Request signatures | Event signatures |
|---|---|---|
| `xdg_wm_base` | `""`, `"n"`, `"no"`, `"u"` | `"u"` (ping) |
| `xdg_surface` | `""`, `"n"`, `"?n?oo"`, `"iiii"`, `"u"` | `"u"` (configure) |
| `xdg_toplevel` | `""`, `"?o"`, `"s"`, `"s"`, … | `"iia"`, `""`, `"ii"`, `"a"` |

```cpp
// in namespace xdg_shell::client:
const wl_interface& xdg_wm_base_traits::wl_iface()  noexcept { return xdg_wm_base_iface_def; }
const wl_interface& xdg_surface_traits::wl_iface()  noexcept { return xdg_surface_iface_def; }
const wl_interface& xdg_toplevel_traits::wl_iface() noexcept { return xdg_toplevel_iface_def; }
```

---

## 6 · EGL Setup Sequence (`App::InitEgl()`)

```
1.  eglGetDisplay((EGLNativeDisplayType)display_)  → egl_display_
    ↳ return false if EGL_NO_DISPLAY

2.  eglInitialize(egl_display_, &major, &minor)
    ↳ return false on EGL_FALSE; log "EGL {major}.{minor}"

3.  eglBindAPI(EGL_OPENGL_ES_API)
    ↳ return false on EGL_FALSE

4.  eglChooseConfig with attribs:
        EGL_SURFACE_TYPE=EGL_WINDOW_BIT
        EGL_RENDERABLE_TYPE=EGL_OPENGL_ES2_BIT
        EGL_RED/GREEN/BLUE/ALPHA_SIZE=8
        EGL_DEPTH_SIZE=24
        EGL_NONE
    ↳ return false if num_configs < 1

5.  eglCreateContext with: EGL_CONTEXT_CLIENT_VERSION=2
    → egl_context_
    ↳ return false if EGL_NO_CONTEXT

6.  egl_window_ = wl_egl_window_create(
        reinterpret_cast<wl_surface*>(surface_proxy_), width_, height_)
    ↳ return false if nullptr

7.  eglCreateWindowSurface(egl_display_, egl_config_,
        (EGLNativeWindowType)egl_window_, nullptr)
    → egl_surface_
    ↳ return false if EGL_NO_SURFACE

8.  eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)
    ↳ return false on EGL_FALSE
```

All bail paths call `CleanupEgl()` (reverse teardown) before returning `false`.

---

## 7 · Render Loop Design (`App::MainLoop()`)

```cpp
void App::MainLoop() {
    while (running_) {
        // Dispatch pending events (non-blocking)
        if (wl_display_dispatch_pending(display_) < 0) break;
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) break;

        // Block until first xdg_surface.configure is received
        if (!configured_) {
            wl_display_dispatch(display_);
            continue;
        }

        // Colour-cycling render
        float r = static_cast<float>(frame_count_ % 256) / 255.0f;
        glClearColor(r, 0.3f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(egl_display_, egl_surface_);  // implicitly commits surface
        ++frame_count_;
    }
}
```

`eglSwapBuffers` implicitly calls `wl_surface_commit`, so no manual commit call is needed.

---

## 8 · `examples/simple-egl/meson.build`

```meson
# ── Guard ──────────────────────────────────────────────────────────────────────
egl_dep         = dependency('egl',               required: false)
glesv2_dep      = dependency('glesv2',            required: false)
wayland_egl_dep = dependency('wayland-egl',       required: false)
wp_dep          = dependency('wayland-protocols',  required: false)
wl_scanner_dep  = dependency('wayland-scanner',   required: false)

if not (egl_dep.found() and glesv2_dep.found() and
        wayland_egl_dep.found() and wp_dep.found() and wl_scanner_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ────────────────────────────────────────────────
wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

# ── Generate client headers ────────────────────────────────────────────────────
wayland_client_hdr = custom_target('simple-egl-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'])

xdg_shell_client_hdr = custom_target('simple-egl-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'])

# ── Include paths ──────────────────────────────────────────────────────────────
framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ─────────────────────────────────────────────────────────────────
executable('simple_egl',
    sources: ['main.cpp', wayland_client_hdr, xdg_shell_client_hdr],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep, egl_dep, glesv2_dep, wayland_egl_dep],
    cpp_args: ['-Wno-unused-parameter'])
```

No `test()` registration — requires a live compositor; not suitable for headless CI.

---

## 9 · `examples/meson.build` Update

```meson
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

subdir('minimal')
subdir('simple-egl')   # ← add; meson.build inside uses subdir_done() if deps absent
```

No change to the root `meson.build` needed — the existing `if get_option('examples') and wayland_client_dep.found() and wayland_server_dep.found()` guard already gates all examples.

---

## 10 · Implementation Order

1. **`examples/simple-egl/meson.build`** — build system plumbing first; verify `meson setup` succeeds.
2. **`examples/meson.build`** — add `subdir('simple-egl')`.
3. **`main.cpp` skeleton** — `#include` chain, empty `App` struct, `main()`. Verify it compiles.
4. **`wl_iface()` definitions** — add to `main.cpp`; fix any missing-symbol errors before adding logic.
5. **CRTP handler structs** — add one at a time (`XdgWmBaseHandler` → `XdgSurfaceHandler` → `XdgToplevelHandler` → `SeatHandler` → `KeyboardHandler`), verifying compilation at each step.
6. **`ConnectDisplay()` + `ScanGlobals()` + `BindGlobals()`** — Wayland connection and global binding.
7. **`CreateSurfaces()`** — surface + xdg_surface + xdg_toplevel creation + first `wl_display_roundtrip()`.
8. **`InitEgl()`** — EGL setup; test with `WAYLAND_DISPLAY` set.
9. **`MainLoop()`** — color-cycling `glClear` / `eglSwapBuffers` render loop.
10. **`~App()` cleanup** — verify clean teardown with ASan or valgrind.

---

## Notes & Gotchas

- **`wayland.xml` is large** (~3500 lines, 30+ interfaces). The scanner generates all of them. Only the four used interfaces need `wl_iface()` definitions — linker errors will identify any that are missing.
- **`xdg_toplevel::wm_capabilities`** (version 4 addition) — default no-op override is sufficient unless UI adaptation is desired.
- **`wl_keyboard::keymap` leaks an fd** — `KeyboardHandler` must override `OnKeymap(uint32_t, int32_t fd, uint32_t)` and call `close(fd)` to avoid fd exhaustion.
- **`wl_compositor` and `wl_surface` have no events** — never use CRTP wrappers for them; store as raw `wl_proxy*`.
