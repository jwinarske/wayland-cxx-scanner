# imgui — Dear ImGui platform backend for Wayland

A Dear ImGui **platform backend** built on this project's generated CRTP
bindings and its `wl/` framework, plus a demo application driving it. The
counterpart to `imgui_impl_glfw` / `imgui_impl_sdl3`: it owns input, while the
application keeps the display, surface, xdg-shell and event loop. Rendering is
upstream's `imgui_impl_opengl3` over EGL / OpenGL ES 2.

## Building

Disabled by default, because enabling it fetches Dear ImGui:

```sh
meson setup build -Dexamples=true -Dimgui_examples=enabled
ninja -C build
./build/examples/imgui/imgui_demo_wayland
```

`subprojects/imgui.wrap` fetches the pinned release and applies the build
definition in `subprojects/packagefiles/imgui`, which compiles ImGui core plus
`imgui_impl_opengl3` for GLES 2 — and no platform backend, since that is what
this example is. A system `imgui`, if one is ever packaged, is used instead.

The protocol headers are generated at build time from the system `wayland.xml`
and `xdg-shell.xml`, exactly like the other examples.

## What the backend covers

| Area | Implementation |
|---|---|
| Mouse | `BackendPointer` on the generated `CWlPointer<T>`: continuous `axis`, `axis_discrete` (v5–7), hi-res `axis_value120` (v8+), batched on `wl_pointer.frame`; BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA; surface-filtered enter/leave |
| Keyboard | `wl::KeyboardHandler` as-is: xkbcommon keymap, timerfd key repeat (plus wl_keyboard v10 server-driven repeat), evdev→`ImGuiKey` table, keysym→UTF-32 text input, per-side modifier tracking, focus loss via `OnKeyboardLeave` |
| Touch | `wl::TouchHandler` hooks, single-touch mouse emulation reported as `ImGuiMouseSource_TouchScreen` |
| Cursor | `wl::CursorManager` — XDG cursor-spec shape names for all 11 `ImGuiMouseCursor_` values, animated cursors, HiDPI theme reload, `MouseDrawCursor`/`None` → `set_cursor(null)` |
| Lifecycle | Backend-private `wl_registry` on the app's connection; versioned `release` teardown; seat is optional (headless/kiosk safe) |

Not wired: clipboard (`wl::DataDevice`), IME/compose (`zwp_text_input_v3` — text
input is per-keysym), multi-viewport and `SetMousePos` (both impossible on
Wayland).

`BackendPointer` predates `wl::PointerHandler`'s scroll hooks and could now use
them, except that it filters enter/leave by surface and `wl::PointerEvent` does
not carry the surface. See the note above it.

## Integration contract

The application owns the display, surface, xdg-shell and event loop; the backend
owns input. Three touch points:

1. **Geometry** — forward `xdg_toplevel.configure` sizes with
   `ImGui_ImplWaylandCxx_SetDisplaySize()`, and buffer scale with
   `SetContentScale()` (feeds `DisplayFramebufferScale` and reloads the cursor
   theme).
2. **Key repeat / animated cursors** — two timerfds to add to your poll set.
   `wl::RunEventLoop`'s 5-argument overload handles the keyboard one directly
   (see `main.cpp`).
3. **`wl_iface()` ownership** — `<wl/seat.hpp>` defines the traits for every
   interface it binds (`wl_seat`, `wl_keyboard`, `wl_pointer`, `wl_touch`); this
   backend adds `wl_shm`, which it binds itself for the cursor. Don't define
   `wl_shm` again in application code; the app keeps `wl_compositor`,
   `wl_surface`, `wl_callback`, and anything else it binds.

```cpp
ImGui::CreateContext();
ImGui_ImplWaylandCxx_Init(display, surface, compositor);  // 2 bounded roundtrips
ImGui_ImplOpenGL3_Init("#version 100");

// per frame (e.g. from your wl_callback.done handler):
ImGui_ImplOpenGL3_NewFrame();
ImGui_ImplWaylandCxx_NewFrame();   // DisplaySize, DeltaTime, cursor shape
ImGui::NewFrame();
```
