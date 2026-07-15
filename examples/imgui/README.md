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
| Mouse | `wl::PointerHandler` hooks: position, BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA, and the normalized axis family — continuous `axis`, `axis_discrete` (v5–7) and hi-res `axis_value120` (v8+) all arrive as `value120`, batched on `wl_pointer.frame`; enter/leave filtered to the application's surface |
| Keyboard | `wl::KeyboardHandler` as-is: xkbcommon keymap, timerfd key repeat (plus wl_keyboard v10 server-driven repeat), evdev→`ImGuiKey` table, keysym→UTF-32 text input, per-side modifier tracking, focus loss via `OnKeyboardLeave` |
| IME | `wl::ime` facade (whichever backend `ime_backend` selects; compiled out for `none`): `Platform_SetImeDataFn` drives enable/disable and the cursor rectangle, `commit_string` → `AddInputCharactersUTF8`, `delete_surrounding_text` → synthesized Backspace. Composition text is tracked but not drawn — ImGui has no preedit API — and the keysym text path stands down while it is non-empty so composed characters are not typed twice. Dead keys / compose sequences are not composed by the backend itself |
| Touch | `wl::TouchHandler` hooks, single-touch mouse emulation reported as `ImGuiMouseSource_TouchScreen` |
| Cursor | `wl::CursorManager` — XDG cursor-spec shape names for all 11 `ImGuiMouseCursor_` values, animated cursors, HiDPI theme reload, `MouseDrawCursor`/`None` → `set_cursor(null)` |
| Clipboard | `wl::DataDevice` on the core `wl_data_device`: copy publishes a source quoting the latest key/button serial (the compositor requires one), paste reads the offered pipe with a 500 ms / 1 MiB bound so a stalled peer cannot hang the frame. A paste of our own selection is answered locally — reading it would deadlock against our own `OnSend` |
| Lifecycle | Backend-private `wl_registry` on the app's connection; versioned `release` teardown; seat is optional (headless/kiosk safe) |

Deliberately not wired, and why:

- **Multi-viewport** and **`SetMousePos`** — impossible on Wayland: it has no
  global coordinates, and a client cannot warp the pointer.
- **OS drag-and-drop** — nothing to wire it to. ImGui's drag-and-drop
  (`BeginDragDropSource` / `AcceptDragDropPayload`) moves payloads between ImGui
  windows inside one context; there is no platform hook for a drag that crosses
  the process boundary. `wl::DataDevice` is selection-only to match, so both
  ends would need building. Worth revisiting only if ImGui grows a platform API
  for it.

## Integration contract

The application owns the display, surface, xdg-shell and event loop; the backend
owns input. Three touch points:

1. **Geometry** — forward `xdg_toplevel.configure` sizes with
   `ImGui_ImplWaylandCxx_SetDisplaySize()`, and buffer scale with
   `SetContentScale()` (feeds `DisplayFramebufferScale` and reloads the cursor
   theme). Scale is the application's to source, since it owns the surface; the
   demo binds `wp_fractional_scale_v1` + `wp_viewport` and forwards
   `preferred_scale / 120` — see **Scale** below.
2. **Key repeat / animated cursors** — two timerfds to add to your poll set.
   `wl::RunEventLoop`'s 5-argument overload handles the keyboard one directly
   (see `main.cpp`).
3. **`wl_iface()` ownership** — `<wl/seat.hpp>` defines the traits for every
   interface it binds (`wl_seat`, `wl_keyboard`, `wl_pointer`, `wl_touch`); this
   backend adds what it binds itself: `wl_shm` for the cursor and
   `wl_data_device_manager` / `wl_data_device` / `wl_data_offer` /
   `wl_data_source` for the clipboard. Don't define those again in application
   code; the app keeps `wl_compositor`, `wl_surface`, `wl_callback`, and
   anything else it binds.

```cpp
ImGui::CreateContext();
ImGui_ImplWaylandCxx_Init(display, surface, compositor);  // 2 bounded roundtrips
ImGui_ImplOpenGL3_Init("#version 100");

// per frame (e.g. from your wl_callback.done handler):
ImGui_ImplOpenGL3_NewFrame();
ImGui_ImplWaylandCxx_NewFrame();   // DisplaySize, DeltaTime, cursor shape
ImGui::NewFrame();
```

## Scale

The demo owns the surface, so it owns scale. It binds `wp_fractional_scale_v1`
and `wp_viewport` and applies the framework's normative policy
(`<wl/scale_policy.hpp>`): the buffer is allocated at **physical** pixels, the
viewport destination is the **logical** size, and ImGui lays out in logical
units with `DisplayFramebufferScale` carrying the rest.

Two details that are easy to get wrong:

- **The GL viewport comes from `ScalePolicy`, not from
  `DisplaySize * DisplayFramebufferScale`.** That product truncates where the
  policy rounds half up, so at a fractional scale the two can disagree by one
  pixel — leaving a seam along an edge the buffer has but the viewport does not
  cover. Both sizes must come from the same rounding.
- **The two protocols are bound as a pair or not at all.** A fractional scale
  with no viewport to present the physical buffer at the logical size would
  simply size the window wrong, so a compositor offering only one gets unity.

Without `wp_fractional_scale_v1` the window stays at scale 1 and the compositor
upscales it. There is no integer fallback: `wl_surface.preferred_buffer_scale`
needs `wl_compositor` v6, and compositors that lack fractional-scale here also
advertise v5 — so the fallback the protocol offers is not available on the
compositors that would need it. Doing it properly means tracking
`wl_output.scale` across `wl_surface.enter`/`leave`, which no example here
needs yet.
