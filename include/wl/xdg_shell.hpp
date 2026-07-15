// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// xdg-shell — header-only wl_interface tables and generic CRTP handler
// templates for the XDG shell protocol (xdg-shell.xml, version 7).
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated xdg_shell_client.hpp:
//
//   #include "xdg_shell_client.hpp"  // defines CXdgWmBase, CXdgSurface, …
//   #include <wl/xdg_shell.hpp>      // tables + wl_iface() impls + handlers
//
// ── Provided utilities
// ────────────────────────────────────────────────────────
//
// Interface tables (namespace wl::xdg, version 7):
//   Inline wl_interface objects for all five xdg-shell interfaces plus the
//   supporting wl_message arrays.  These replace the ~150-line boilerplate
//   block that every xdg-shell example used to reproduce verbatim.
//
// wl_iface() implementations (namespace xdg_shell::client):
//   Inline out-of-line definitions of the pure-virtual wl_iface() methods
//   declared in each generated traits struct (xdg_shell_client.hpp).
//
// Generic CRTP handler templates (namespace wl):
//   wl::XdgWmBaseHandler        — responds to ping automatically (no App param)
//   wl::XdgSurfaceHandler<App>  — acks configure, calls OnXdgSurfaceConfigure()
//   wl::XdgToplevelHandler<App> — delegates configure/close/bounds/caps to App
//
//   App must expose:
//     void OnXdgSurfaceConfigure(uint32_t serial);
//     void OnToplevelConfigure(int32_t w, int32_t h);  // no-op if fixed size
//     void OnToplevelClose();
//   App may optionally expose:
//     void OnToplevelStates(const wl::ToplevelStates&);  // decoded configure
//                                                        // states; detected
//                                                        // via SFINAE
#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-protocol.h>
}

#include <cstddef>   // std::size_t
#include <cstdint>   // uint32_t
#include <iterator>  // std::data
#include <utility>   // std::declval

// ══════════════════════════════════════════════════════════════════════════════
// xdg-shell wl_interface definitions (version 7)
//
// There is no pre-built system symbol for the xdg-shell interfaces (unlike
// core Wayland).  We reproduce the exact same tables that the C
// wayland-scanner generates from xdg-shell.xml (version 7) so that
// libwayland can type-check and dispatch correctly.
//
// All variables are `inline` so each definition is a single instance across
// all translation units that include this header (ODR-safe, C++17 §9.2.6).
// ══════════════════════════════════════════════════════════════════════════════

namespace wl::xdg {

// ── Forward declarations
// ────────────────────────────────────────────────────── Needed so that types[]
// can take the addresses of the five interfaces before any of their definitions
// appear.

extern const wl_interface wm_base_iface;
extern const wl_interface positioner_iface;
extern const wl_interface surface_iface;
extern const wl_interface toplevel_iface;
extern const wl_interface popup_iface;

// ── Shared pointer array
// ────────────────────────────────────────────────────── Mirrors
// xdg_shell_types[] from xdg-shell-protocol.c (C wayland-scanner v7).
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
    nullptr,                // [0]  scalar / no-type slots
    nullptr,                // [1]
    nullptr,                // [2]
    nullptr,                // [3]
    &positioner_iface,      // [4]  create_positioner → new_id
    &surface_iface,         // [5]  get_xdg_surface   → new_id
    &wl_surface_interface,  // [6]  get_xdg_surface   → surface object
    &toplevel_iface,        // [7]  get_toplevel       → new_id
    &popup_iface,           // [8]  get_popup          → new_id
    &surface_iface,         // [9]  get_popup          → parent (?o)
    &positioner_iface,      // [10] get_popup          → positioner
    &toplevel_iface,        // [11] set_parent         → ?o
    &wl_seat_interface,     // [12] show_window_menu   → seat
    nullptr,                // [13] show_window_menu   → serial
    nullptr,                // [14] show_window_menu   → x
    nullptr,                // [15] show_window_menu   → y
    &wl_seat_interface,     // [16] move               → seat
    nullptr,                // [17] move               → serial
    &wl_seat_interface,     // [18] resize             → seat
    nullptr,                // [19] resize             → serial
    nullptr,                // [20] resize             → edges
    &wl_output_interface,   // [21] set_fullscreen     → output (?o)
    &wl_seat_interface,     // [22] grab               → seat
    nullptr,                // [23] grab               → serial
    &positioner_iface,      // [24] reposition         → positioner
    nullptr,                // [25] reposition         → token
};

// kScalars points at the null-filled head of types[]; used by scalar-only
// messages.  &types[0] is a constant expression (address of a static-storage-
// duration object) so kScalars can be constexpr.
inline constexpr const wl_interface** kScalars = &types[0];

// ── Message tables
// ────────────────────────────────────────────────────────────

inline constexpr wl_message wm_base_requests[] = {
    {"destroy", "", nullptr},
    {"create_positioner", "n", &types[4]},
    {"get_xdg_surface", "no", &types[5]},
    {"pong", "u", kScalars},
};
inline constexpr wl_message wm_base_events[] = {
    {"ping", "u", kScalars},
};

inline constexpr wl_message positioner_requests[] = {
    {"destroy", "", nullptr},
    {"set_size", "ii", kScalars},
    {"set_anchor_rect", "iiii", kScalars},
    {"set_anchor", "u", kScalars},
    {"set_gravity", "u", kScalars},
    {"set_constraint_adjustment", "u", kScalars},
    {"set_offset", "ii", kScalars},
    {"set_reactive", "3", nullptr},  // version tag only
    {"set_parent_size", "3ii", kScalars},
    {"set_parent_configure", "3u", kScalars},
};

inline constexpr wl_message surface_requests[] = {
    {"destroy", "", nullptr},         {"get_toplevel", "n", &types[7]},
    {"get_popup", "n?oo", &types[8]}, {"set_window_geometry", "iiii", kScalars},
    {"ack_configure", "u", kScalars},
};
inline constexpr wl_message surface_events[] = {
    {"configure", "u", kScalars},
};

inline constexpr wl_message toplevel_requests[] = {
    {"destroy", "", nullptr},
    {"set_parent", "?o", &types[11]},
    {"set_title", "s", kScalars},
    {"set_app_id", "s", kScalars},
    {"show_window_menu", "ouii", &types[12]},
    {"move", "ou", &types[16]},
    {"resize", "ouu", &types[18]},
    {"set_max_size", "ii", kScalars},
    {"set_min_size", "ii", kScalars},
    {"set_maximized", "", nullptr},
    {"unset_maximized", "", nullptr},
    {"set_fullscreen", "?o", &types[21]},
    {"unset_fullscreen", "", nullptr},
    {"set_minimized", "", nullptr},
};
inline constexpr wl_message toplevel_events[] = {
    {"configure", "iia", kScalars},
    {"close", "", nullptr},
    {"configure_bounds", "4ii", kScalars},
    {"wm_capabilities", "5a", kScalars},
};

inline constexpr wl_message popup_requests[] = {
    {"destroy", "", nullptr},
    {"grab", "ou", &types[22]},
    {"reposition", "3ou", &types[24]},
};
inline constexpr wl_message popup_events[] = {
    {"configure", "iiii", kScalars},
    {"popup_done", "", nullptr},
    {"repositioned", "3u", kScalars},
};

// ── Interface object definitions
// ────────────────────────────────────────────── std::data() converts the
// constexpr array to a const wl_message* pointer without triggering the
// array-decay diagnostic.
// clang-format off
inline const wl_interface wm_base_iface = {
    "xdg_wm_base",    7,
    4,  std::data(wm_base_requests),    1, std::data(wm_base_events)};
inline const wl_interface positioner_iface = {
    "xdg_positioner", 7,
    10, std::data(positioner_requests), 0, nullptr};
inline const wl_interface surface_iface = {
    "xdg_surface",    7,
    5,  std::data(surface_requests),    1, std::data(surface_events)};
inline const wl_interface toplevel_iface = {
    "xdg_toplevel",   7,
    14, std::data(toplevel_requests),   4, std::data(toplevel_events)};
inline const wl_interface popup_iface = {
    "xdg_popup",      7,
    3,  std::data(popup_requests),      3, std::data(popup_events)};
// clang-format on

// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

}  // namespace wl::xdg

// ══════════════════════════════════════════════════════════════════════════════
// xdg_shell::client traits — wl_iface() inline implementations
//
// Provide the out-of-line definitions of the pure-virtual wl_iface() methods
// declared by the generated xdg_shell_client.hpp.  That header must be
// included before this one so these definitions see the complete traits types.
// ══════════════════════════════════════════════════════════════════════════════

namespace xdg_shell::client {

inline const wl_interface& xdg_wm_base_traits::wl_iface() noexcept {
  return wl::xdg::wm_base_iface;
}
inline const wl_interface& xdg_positioner_traits::wl_iface() noexcept {
  return wl::xdg::positioner_iface;
}
inline const wl_interface& xdg_surface_traits::wl_iface() noexcept {
  return wl::xdg::surface_iface;
}
inline const wl_interface& xdg_toplevel_traits::wl_iface() noexcept {
  return wl::xdg::toplevel_iface;
}
inline const wl_interface& xdg_popup_traits::wl_iface() noexcept {
  return wl::xdg::popup_iface;
}

}  // namespace xdg_shell::client

// ══════════════════════════════════════════════════════════════════════════════
// Generic XDG CRTP handler templates (namespace wl)
//
// Ready-to-use CRTP handlers for the standard XDG shell event patterns.
// Include the generated xdg_shell_client.hpp before this header so that
// CXdgWmBase<>, CXdgSurface<>, and CXdgToplevel<> are visible.
//
// Usage example:
//
//   class App {
//     wl::WlPtr<wl::XdgWmBaseHandler>        xdg_wm_base_;
//     wl::WlPtr<wl::XdgSurfaceHandler<App>>  xdg_surface_;
//     wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;
//     …
//     void OnXdgSurfaceConfigure(uint32_t serial) { configured_ = true; }
//     void OnToplevelConfigure(int32_t w, int32_t h) { /* resize */ }
//     void OnToplevelClose()                         { running_ = false; }
//   };
// ══════════════════════════════════════════════════════════════════════════════

namespace wl {

/// XDG wm_base handler — responds to compositor ping automatically.
///
/// Does not need an App back-pointer: the only event is ping, which is handled
/// by calling Pong() immediately.
class XdgWmBaseHandler
    : public xdg_shell::client::CXdgWmBase<XdgWmBaseHandler> {
 public:
  void OnPing(uint32_t serial) override { Pong(serial); }
};

/// XDG surface handler — acks every configure and delegates to App.
///
/// Calls this->AckConfigure(serial) then app_->OnXdgSurfaceConfigure(serial).
///
/// @tparam App  Application class providing OnXdgSurfaceConfigure(uint32_t).
template <typename App>
class XdgSurfaceHandler
    : public xdg_shell::client::CXdgSurface<XdgSurfaceHandler<App>> {
 public:
  App* app_ = nullptr;

  void OnConfigure(uint32_t serial) override {
    this->AckConfigure(serial);
    app_->OnXdgSurfaceConfigure(serial);
  }
};

/// Decoded xdg_toplevel.configure `states` array.
///
/// The wire form is an array of enum values; this is that array answered as
/// questions, so a consumer neither walks a wl_array nor repeats the decode.
///
/// Only the states from xdg-shell v1 and v2 are decoded.  Later additions
/// (suspended, constrained_*) are deliberately absent: naming them here would
/// make this header require a newer wayland-protocols than the project builds
/// against, and nothing here needs them.  Unknown states are ignored.
struct ToplevelStates {
  bool maximized = false;    ///< Compositor has maximized the window.
  bool fullscreen = false;   ///< Compositor has made the window fullscreen.
  bool resizing = false;     ///< An interactive resize is in progress.
  bool activated = false;    ///< Window is the active one.  This — not keyboard
                             ///< focus — is what drives active vs backdrop
                             ///< styling.
  bool tiled_left = false;   ///< Edge is tiled against something.
  bool tiled_right = false;  ///< Edge is tiled against something.
  bool tiled_top = false;    ///< Edge is tiled against something.
  bool tiled_bottom = false;  ///< Edge is tiled against something.
};

/// XDG toplevel handler — delegates configure and close to App; ignores bounds
/// and wm_capabilities (suitable for most simple applications).
///
/// @tparam App  Application class providing OnToplevelConfigure(int32_t,
/// int32_t)
///              and OnToplevelClose().  May optionally provide
///              OnToplevelStates(const wl::ToplevelStates&), which is called
///              before OnToplevelConfigure so the size can be interpreted in
///              light of the new state.
template <typename App>
class XdgToplevelHandler
    : public xdg_shell::client::CXdgToplevel<XdgToplevelHandler<App>> {
 public:
  App* app_ = nullptr;

  void OnConfigure(int32_t w, int32_t h, wl_array* states) override {
    CallOnToplevelStates(DecodeStates(states), 0);
    app_->OnToplevelConfigure(w, h);
  }
  void OnClose() override { app_->OnToplevelClose(); }
  void OnConfigureBounds(int32_t /*w*/, int32_t /*h*/) override {}
  void OnWmCapabilities(wl_array* /*caps*/) override {}

 private:
  /// Decode the wl_array of xdg_toplevel_state values.
  [[nodiscard]] static ToplevelStates DecodeStates(wl_array* states) noexcept {
    using St = xdg_shell::client::XdgToplevelState;
    ToplevelStates out;
    if (states == nullptr || states->data == nullptr)
      return out;

    const auto* first = static_cast<const uint32_t*>(states->data);
    const std::size_t count = states->size / sizeof(uint32_t);
    for (std::size_t i = 0; i < count; ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      switch (static_cast<St>(first[i])) {
        case St::Maximized:
          out.maximized = true;
          break;
        case St::Fullscreen:
          out.fullscreen = true;
          break;
        case St::Resizing:
          out.resizing = true;
          break;
        case St::Activated:
          out.activated = true;
          break;
        case St::TiledLeft:
          out.tiled_left = true;
          break;
        case St::TiledRight:
          out.tiled_right = true;
          break;
        case St::TiledTop:
          out.tiled_top = true;
          break;
        case St::TiledBottom:
          out.tiled_bottom = true;
          break;
        default:
          break;  // A state this build does not decode.
      }
    }
    return out;
  }

  // ── SFINAE optional toplevel-states hook ─────────────────────────────────

  // Call app_->OnToplevelStates(states) if the method exists.  An int/long
  // priority tag disambiguates rather than an ellipsis fallback: passing a
  // class type through `...` is only conditionally supported.
  template <typename A = App>
  auto CallOnToplevelStates(const ToplevelStates& states, int)
      -> decltype(std::declval<A&>().OnToplevelStates(states), void()) {
    app_->OnToplevelStates(states);
  }
  // Fallback: OnToplevelStates not present — do nothing.
  template <typename A = App>
  void CallOnToplevelStates(const ToplevelStates& /*states*/, long) noexcept {}
};

}  // namespace wl
