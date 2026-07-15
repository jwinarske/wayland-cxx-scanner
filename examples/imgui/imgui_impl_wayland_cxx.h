// dear imgui: Platform Backend for Wayland (via wayland-cxx-scanner)
// This needs to be used along with a Renderer (e.g. OpenGL3 via EGL, Vulkan)
//
// Implemented features:
//  [X] Platform: Mouse support (position, buttons, wheel incl. hi-res
//      value120 / discrete / continuous axis, wl_pointer.frame batching).
//  [X] Platform: Keyboard support via xkbcommon (keysym-resolved text input,
//      compositor-advertised key repeat via wl::KeyboardHandler timerfd,
//      wl_keyboard v10 server-driven repeat).
//  [X] Platform: Touch support (single-touch mouse emulation, reported as
//      ImGuiMouseSource_TouchScreen).
//  [X] Platform: Mouse cursor shapes via wl_cursor themes
//      (io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors), animated
//      cursors included.
//  [X] Platform: Keyboard focus loss (wl_keyboard.leave -> io.AddFocusEvent).
//  [X] Platform: IME via the wl::ime facade (the backend the ime_backend build
//      option selects; compiled out entirely for ime_backend=none).
//      Platform_SetImeDataFn drives enable/disable + cursor rectangle,
//      commit_string feeds AddInputCharactersUTF8, delete_surrounding_text is
//      synthesized as Backspace.
//  [X] Platform: Clipboard via wl_data_device (wl::DataDevice) — copy publishes
//      a data source quoting the latest input serial; paste reads the offered
//      pipe (bounded: 500 ms, 1 MiB).  Absent wl_data_device_manager leaves
//      Ctrl-C/V inert rather than failing.
// Missing features / issues:
//  [ ] Platform: IME composition text is tracked but not drawn (ImGui has no
//      preedit API) and is not exposed, so an application cannot render it
//      either.  Dead keys / compose sequences are likewise not composed by the
//      backend; ordinary text is per-keysym via xkb_keysym_to_utf32().
//  [ ] Platform: Multi-viewport (impossible: Wayland has no global coords).
//  [ ] Platform: SetMousePos (impossible: Wayland cannot warp the pointer).
//
// The backend is self-contained: given a connected wl_display and the app's
// wl_surface it creates its OWN wl_registry on the same connection and binds
// wl_seat / wl_shm itself.  The application keeps full ownership of the
// display, the surface, xdg-shell, and the event loop; the backend's proxies
// are dispatched by the same wl_display_dispatch the app already runs.
//
// Because the compositor delivers surface geometry to the application
// (xdg_toplevel.configure), the app must forward it:
//   ImGui_ImplWaylandCxx_SetDisplaySize(w, h)   from its configure handler
//   ImGui_ImplWaylandCxx_SetContentScale(s)     when buffer scale changes
//
// Keyboard repeat and animated cursors are timerfd-driven.  Add the two fds
// below to the poll set of your event loop (wl::RunEventLoop's repeat-fd
// overload handles the keyboard one directly):
//   ImGui_ImplWaylandCxx_GetKeyRepeatFd()    -> ImGui_ImplWaylandCxx_DispatchKeyRepeat()
//   ImGui_ImplWaylandCxx_GetCursorFrameFd()  -> ImGui_ImplWaylandCxx_DispatchCursorFrame()
//
// Link-time note: <wl/seat.hpp> defines the out-of-line wl_iface() traits for
// every interface it binds — wl_seat, wl_keyboard, wl_pointer, wl_touch — and
// this backend's .cpp adds wl_shm, which it binds itself for the cursor.  Do
// not define wl_shm again in application code.

#pragma once
#include "imgui.h"  // IMGUI_IMPL_API
#ifndef IMGUI_DISABLE

struct wl_display;
struct wl_surface;
struct wl_compositor;

// Follows the standard imgui backend lifecycle.  Init() performs two
// bounded roundtrips (registry scan + seat capabilities) and returns false
// on timeout or bind failure.  `display` must already be connected and
// `surface` created; both must outlive the backend.
// `compositor` is used only to create the cursor's own wl_surface; pass
// nullptr to disable cursor-shape support (e.g. touch-only kiosk targets).
IMGUI_IMPL_API bool ImGui_ImplWaylandCxx_Init(wl_display* display,
                                              wl_surface* surface,
                                              wl_compositor* compositor);
IMGUI_IMPL_API void ImGui_ImplWaylandCxx_Shutdown();
IMGUI_IMPL_API void ImGui_ImplWaylandCxx_NewFrame();

// Surface geometry, forwarded by the application (logical/surface-local
// coordinates, i.e. the values from xdg_toplevel.configure).
IMGUI_IMPL_API void ImGui_ImplWaylandCxx_SetDisplaySize(int logical_w,
                                                        int logical_h);
// Buffer scale (integer wl_surface.set_buffer_scale value, or a fractional
// scale if the app implements wp_fractional_scale_v1).  Feeds
// io.DisplayFramebufferScale and reloads the cursor theme at the new size.
IMGUI_IMPL_API void ImGui_ImplWaylandCxx_SetContentScale(float scale);

// Keyboard-repeat timerfd plumbing (-1 when no keyboard is bound or repeat
// setup failed; re-query every loop iteration, capability can change).
IMGUI_IMPL_API int ImGui_ImplWaylandCxx_GetKeyRepeatFd();
IMGUI_IMPL_API void ImGui_ImplWaylandCxx_DispatchKeyRepeat();

// Animated-cursor timerfd plumbing (-1 when inactive).
IMGUI_IMPL_API int ImGui_ImplWaylandCxx_GetCursorFrameFd();
IMGUI_IMPL_API void ImGui_ImplWaylandCxx_DispatchCursorFrame();

#endif  // #ifndef IMGUI_DISABLE
