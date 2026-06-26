// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// simple_shell — header-only wl_interface tables and generic CRTP handler
// template for the RDK / Westeros simple_shell protocol (simpleshell.xml,
// wl_simple_shell version 1).
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated simple_shell_client.hpp:
//
//   #include "simple_shell_client.hpp"  // defines CWlSimpleShell, traits
//   #include <wl/simple_shell.hpp>      // tables + wl_iface() impl + handler
//
// ── Provided utilities
// ────────────────────────────────────────────────────────
//
// Interface tables (namespace wl::simpleshell, version 1):
//   Inline wl_interface object for wl_simple_shell plus the supporting
//   wl_message arrays.  wl_simple_shell is RDK-specific and not part of
//   wayland-protocols, so the wl_interface is defined inline here.
//
// wl_iface() implementation (namespace simple_shell::client):
//   Inline out-of-line definition of the pure-virtual wl_iface() method
//   declared in the generated wl_simple_shell_traits struct.
//
// Generic CRTP handler template (namespace wl):
//   wl::SimpleShellHandler<App> — delegates all six events to App via the
//   matching OnSimpleShell* methods:
//     surface_id        → OnSimpleShellSurfaceId
//     surface_created   → OnSimpleShellSurfaceCreated
//     surface_destroyed → OnSimpleShellSurfaceDestroyed
//     surface_status    → OnSimpleShellSurfaceStatus
//     get_surfaces_done → OnSimpleShellGetSurfacesDone
//     popup_details     → OnSimpleShellPopupDetails
#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-protocol.h>
}

#include <iterator>  // std::data

// ══════════════════════════════════════════════════════════════════════════════
// simple_shell wl_interface definitions (wl_simple_shell version 1)
//
// wl_simple_shell originates from RDK's Westeros compositor and has no
// pre-built system symbol (unlike wl_compositor or xdg_wm_base).  We define
// the wl_interface inline here to match the upstream protocol from
// RDK Management's westeros-soc simpleshell.xml.
//
// Protocol layout (version 1):
//   Requests (11): set_name(0), set_visible(1), set_geometry(2),
//   set_opacity(3),
//                  set_zorder(4), get_status(5), get_surfaces(6), set_focus(7),
//                  set_scale(8), get_popup(9), is_surface_popup(10)
//   Events    (6): surface_id(0), surface_created(1), surface_destroyed(2),
//                  surface_status(3), get_surfaces_done(4), popup_details(5)
//
// All variables are `inline` so each translation unit that includes this
// header gets a single, ODR-safe definition (C++17 §9.2.6).
// ══════════════════════════════════════════════════════════════════════════════

namespace wl::simpleshell {

// ── Shared pointer array
// ──────────────────────────────────────────────────────
//
// Only one argument in the whole protocol carries a typed object — the
// `surface` argument of the surface_id event (a wl_surface).  Every other
// argument is scalar / string / fixed, so its slot is nullptr.  Scalar
// messages point at &types[1]; libwayland only dereferences a slot for 'o'/'n'
// signature characters, so the shared run of nullptrs is read-safe for all of
// them (the longest scalar message, surface_status, has 9 args → types[1..9]).
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
    &wl_surface_interface,  // [0] surface_id: surface (wl_surface)
    nullptr,                // [1] generic scalar / nullptr padding
    nullptr,                // [2]
    nullptr,                // [3]
    nullptr,                // [4]
    nullptr,                // [5]
    nullptr,                // [6]
    nullptr,                // [7]
    nullptr,                // [8]
    nullptr,                // [9]
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// ── Message tables
// ────────────────────────────────────────────────────────────
// clang-format off
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
inline constexpr wl_message requests[] = {
    {"set_name",         "us",     &types[1]},  // 0: surfaceId+name
    {"set_visible",      "uu",     &types[1]},  // 1: surfaceId+visible
    {"set_geometry",     "uiiii",  &types[1]},  // 2: surfaceId+x+y+w+h
    {"set_opacity",      "uf",     &types[1]},  // 3: surfaceId+opacity
    {"set_zorder",       "uf",     &types[1]},  // 4: surfaceId+zorder
    {"get_status",       "u",      &types[1]},  // 5: surfaceId
    {"get_surfaces",     "",       nullptr},    // 6: no args
    {"set_focus",        "u",      &types[1]},  // 7: surfaceId
    {"set_scale",        "uff",    &types[1]},  // 8: surfaceId+scaleX+scaleY
    {"get_popup",        "uuiiii", &types[1]},  // 9: surfaceId+parent+x+y+w+h
    {"is_surface_popup", "u",      &types[1]},  // 10: surfaceId
};
inline constexpr wl_message events[] = {
    {"surface_id",        "ou",        &types[0]},  // 0: surface+surfaceId
    {"surface_created",   "us",        &types[1]},  // 1: surfaceId+name
    {"surface_destroyed", "us",        &types[1]},  // 2: surfaceId+name
    {"surface_status",    "usuiiiiff", &types[1]},  // 3: full status
    {"get_surfaces_done", "",          nullptr},    // 4: no args
    {"popup_details",     "uui",       &types[1]},  // 5: surfaceId+parent+popup
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)
// clang-format on

// ── Interface object
// ──────────────────────────────────────────────────────────

// clang-format off
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline const wl_interface shell_iface = {
    "wl_simple_shell", 1,
    11, std::data(requests),
    6,  std::data(events)};
// clang-format on

}  // namespace wl::simpleshell

// ══════════════════════════════════════════════════════════════════════════════
// simple_shell::client traits — wl_iface() inline implementation
//
// Provide the out-of-line definition of the pure-virtual wl_iface() method
// declared by the generated simple_shell_client.hpp.  That header must be
// included before this one so this definition sees the complete traits type.
// ══════════════════════════════════════════════════════════════════════════════

namespace simple_shell::client {

inline const wl_interface& wl_simple_shell_traits::wl_iface() noexcept {
  return wl::simpleshell::shell_iface;
}

}  // namespace simple_shell::client

// ══════════════════════════════════════════════════════════════════════════════
// Generic simple_shell CRTP handler template (namespace wl)
//
// Delegates all six wl_simple_shell events to an application class.  Include
// the generated simple_shell_client.hpp before this header so that
// CWlSimpleShell<> is visible.
//
// Usage example:
//
//   class App {
//     wl::WlPtr<wl::SimpleShellHandler<App>> shell_;
//     …
//     void OnSimpleShellSurfaceId(wl_proxy* surface, uint32_t id);
//     void OnSimpleShellSurfaceCreated(uint32_t id, const char* name);
//     void OnSimpleShellSurfaceDestroyed(uint32_t id, const char* name);
//     void OnSimpleShellSurfaceStatus(uint32_t id, const char* name,
//                                     uint32_t visible, int32_t x, int32_t y,
//                                     int32_t w, int32_t h, wl_fixed_t opacity,
//                                     wl_fixed_t zorder);
//     void OnSimpleShellGetSurfacesDone();
//     void OnSimpleShellPopupDetails(uint32_t id, uint32_t parent,
//                                    int32_t popup);
//   };
// ══════════════════════════════════════════════════════════════════════════════

namespace wl {

/// simple_shell handler — delegates all events to App.
///
/// @tparam App  Application class providing the six OnSimpleShell* methods.
template <typename App>
class SimpleShellHandler
    : public ::simple_shell::client::CWlSimpleShell<SimpleShellHandler<App>> {
 public:
  App* app_ = nullptr;

  void OnSurfaceId(wl_proxy* surface, uint32_t surfaceId) override {
    if (app_)
      app_->OnSimpleShellSurfaceId(surface, surfaceId);
  }

  void OnSurfaceCreated(uint32_t surfaceId, const char* name) override {
    if (app_)
      app_->OnSimpleShellSurfaceCreated(surfaceId, name);
  }

  void OnSurfaceDestroyed(uint32_t surfaceId, const char* name) override {
    if (app_)
      app_->OnSimpleShellSurfaceDestroyed(surfaceId, name);
  }

  void OnSurfaceStatus(uint32_t surfaceId,
                       const char* name,
                       uint32_t visible,
                       int32_t x,
                       int32_t y,
                       int32_t width,
                       int32_t height,
                       wl_fixed_t opacity,
                       wl_fixed_t zorder) override {
    if (app_)
      app_->OnSimpleShellSurfaceStatus(surfaceId, name, visible, x, y, width,
                                       height, opacity, zorder);
  }

  void OnGetSurfacesDone() override {
    if (app_)
      app_->OnSimpleShellGetSurfacesDone();
  }

  void OnPopupDetails(uint32_t surfaceId,
                      uint32_t parentSurfaceId,
                      int32_t popup) override {
    if (app_)
      app_->OnSimpleShellPopupDetails(surfaceId, parentSurfaceId, popup);
  }
};

}  // namespace wl
