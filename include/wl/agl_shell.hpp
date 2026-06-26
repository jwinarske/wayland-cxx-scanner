// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// agl-shell — header-only wl_interface tables and generic CRTP handler
// template for the AGL shell protocol (agl-shell.xml, version 4).
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated agl_shell_client.hpp:
//
//   #include "agl_shell_client.hpp"  // defines CAglShell, agl_shell_traits
//   #include <wl/agl_shell.hpp>      // tables + wl_iface() impl + handler
//
// ── Provided utilities
// ────────────────────────────────────────────────────────
//
// Interface tables (namespace wl::agl, version 4):
//   Inline wl_interface object for agl_shell plus the supporting wl_message
//   arrays.  These replace the ~65-line boilerplate block that the
//   agl-compositor example used to reproduce verbatim.
//
// wl_iface() implementation (namespace agl_shell::client):
//   Inline out-of-line definition of the pure-virtual wl_iface() method
//   declared in the generated agl_shell_traits struct.
//
// Generic CRTP handler template (namespace wl):
//   wl::AglShellHandler<App> — delegates all three events to App:
//     bound_ok   → App::OnAglBoundOk()
//     bound_fail → App::OnAglBoundFail()
//     app_state  → App::OnAglAppState(const char* app_id, uint32_t state)
//
//   App must expose:
//     void OnAglBoundOk();
//     void OnAglBoundFail();
//     void OnAglAppState(const char* app_id, uint32_t state);
#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-protocol.h>
}

#include <iterator>  // std::data

// ══════════════════════════════════════════════════════════════════════════════
// agl-shell wl_interface definitions (version 4)
//
// agl_shell is not part of wayland-protocols and has no pre-built system
// symbol (unlike wl_compositor or xdg_wm_base).  We define the wl_interface
// inline here to match the upstream agl-shell protocol (agl-compositor,
// protocol/agl-shell.xml), vendored in this repo as protocols/agl-shell.xml.
//
// Protocol layout (version 4):
//   Requests (6): ready(0), set_background(1), set_panel(2), activate_app(3),
//                 destroy(4, since v2), set_activate_region(5, since v4)
//   Events   (3): bound_ok(0, since v2), bound_fail(1, since v2),
//                 app_state(2, since v3)
//
// All variables are `inline` so each translation unit that includes this
// header gets a single, ODR-safe definition (C++17 §9.2.6).
// ══════════════════════════════════════════════════════════════════════════════

namespace wl::agl {

// ── Shared pointer array
// ──────────────────────────────────────────────────────
//
// • avoid-non-const-global-variables: element type must be non-const pointer
//   (const wl_interface*) because wl_message::types is const wl_interface**;
//   adding const to the elements would break the implicit conversion.
// • interfaces-global-init: initializers are object addresses (link-time
//   constants), safe regardless of definition order.
// • avoid-c-arrays: mandated by the Wayland C API.
//
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
inline const wl_interface* types[] = {
    nullptr,                // [0]  scalar placeholder
    &wl_surface_interface,  // [1]  set_background/set_panel: surface arg
    &wl_output_interface,   // [2]  set_background/set_panel: output arg
    nullptr,                // [3]  set_panel: edge (uint)
    nullptr,                // [4]  activate_app: app_id (string)
    &wl_output_interface,   // [5]  activate_app: output arg
    &wl_output_interface,   // [6]  set_activate_region: output arg
    nullptr,                // [7]  set_activate_region: x (int)
    nullptr,                // [8]  set_activate_region: y (int)
    nullptr,                // [9]  set_activate_region: width (int)
    nullptr,                // [10] set_activate_region: height (int)
    nullptr,                // [11] app_state: app_id (string)
    nullptr,                // [12] app_state: state (uint)
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// ── Message tables
// ────────────────────────────────────────────────────────────
// clang-format off
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
inline constexpr wl_message requests[] = {
    {"ready",               "",       nullptr},      // 0: v1, no args
    {"set_background",      "oo",     &types[1]},    // 1: v1, surface+output
    {"set_panel",           "oou",    &types[1]},    // 2: v1, surface+output+edge
    {"activate_app",        "so",     &types[4]},    // 3: v1, string+output
    {"destroy",             "2",      nullptr},      // 4: v2 destructor, no args
    {"set_activate_region", "4oiiii", &types[6]},    // 5: v4, output+4×int
};
inline constexpr wl_message events[] = {
    {"bound_ok",   "2",   nullptr},                  // 0: v2, no args
    {"bound_fail", "2",   nullptr},                  // 1: v2, no args
    {"app_state",  "3su", &types[11]},               // 2: v3, string+uint
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)
// clang-format on

// ── Interface object
// ──────────────────────────────────────────────────────────

// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline const wl_interface shell_iface = {
    "agl_shell", 4,
    6, std::data(requests),
    3, std::data(events)};
// clang-format on

}  // namespace wl::agl

// ══════════════════════════════════════════════════════════════════════════════
// agl_shell::client traits — wl_iface() inline implementation
//
// Provide the out-of-line definition of the pure-virtual wl_iface() method
// declared by the generated agl_shell_client.hpp.  That header must be
// included before this one so this definition sees the complete traits type.
// ══════════════════════════════════════════════════════════════════════════════

namespace agl_shell::client {

inline const wl_interface& agl_shell_traits::wl_iface() noexcept {
  return wl::agl::shell_iface;
}

}  // namespace agl_shell::client

// ══════════════════════════════════════════════════════════════════════════════
// Generic AGL shell CRTP handler template (namespace wl)
//
// Delegates all three agl_shell events to an application class.  Include the
// generated agl_shell_client.hpp before this header so that CAglShell<> is
// visible.
//
// Usage example:
//
//   class App {
//     wl::WlPtr<wl::AglShellHandler<App>> agl_shell_;
//     …
//     void OnAglBoundOk()   { /* compositor accepted; proceed */ }
//     void OnAglBoundFail() { /* another shell active; exit */ }
//     void OnAglAppState(const char* app_id, uint32_t state) { /* log */ }
//   };
// ══════════════════════════════════════════════════════════════════════════════

namespace wl {

/// AGL shell handler — delegates all events to App.
///
/// @tparam App  Application class providing OnAglBoundOk(), OnAglBoundFail(),
///              and OnAglAppState(const char*, uint32_t).
template <typename App>
class AglShellHandler
    : public agl_shell::client::CAglShell<AglShellHandler<App>> {
 public:
  App* app_ = nullptr;

  void OnBoundOk() override {
    if (app_)
      app_->OnAglBoundOk();
  }

  void OnBoundFail() override {
    if (app_)
      app_->OnAglBoundFail();
  }

  void OnAppState(const char* app_id, uint32_t state) override {
    if (app_)
      app_->OnAglAppState(app_id, state);
  }
};

}  // namespace wl
