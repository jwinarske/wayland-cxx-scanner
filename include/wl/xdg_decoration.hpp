// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// xdg-decoration — header-only wl_interface tables and generic CRTP handler
// templates for the xdg-decoration-unstable-v1 protocol (version 1).
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated xdg_decoration_unstable_v1_client.hpp
// AND <wl/xdg_shell.hpp> (for the xdg_toplevel interface reference):
//
//   #include "xdg_decoration_unstable_v1_client.hpp"
//   #include <wl/xdg_shell.hpp>
//   #include <wl/xdg_decoration.hpp>
//
// ── Provided utilities
// ────────────────────────────────────────────────────────
//
// Interface tables (namespace wl::xdg_decoration, version 1):
//   Inline wl_interface objects for zxdg_decoration_manager_v1 and
//   zxdg_toplevel_decoration_v1.
//
// wl_iface() implementations (namespace xdg_decoration_unstable_v1::client):
//   Inline out-of-line definitions of the pure-virtual wl_iface() methods
//   declared in each generated traits struct.
//
// Generic CRTP handler templates (namespace wl):
//   wl::XdgDecorationManagerHandler — event-less, provides ProcessEvent stub
//   wl::XdgDecorationHandler<App>   — delegates configure to App
//
//   App must expose:
//     void OnDecorationConfigure(uint32_t mode);
#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-protocol.h>
}

#include <iterator>  // std::data

// Forward-declare the xdg_toplevel wl_interface from xdg_shell.hpp so that
// the types array below can reference it without forcing an include-order
// dependency.  The actual definition lives in <wl/xdg_shell.hpp>.
namespace wl::xdg {
extern const wl_interface toplevel_iface;
}  // namespace wl::xdg

// ══════════════════════════════════════════════════════════════════════════════
// xdg-decoration wl_interface definitions (version 1)
//
// xdg-decoration-unstable-v1 is not yet stable and has no pre-built system
// symbol (unlike xdg-shell).  We reproduce the exact same tables that the C
// wayland-scanner generates from xdg-decoration-unstable-v1.xml so that
// libwayland can type-check and dispatch correctly.
//
// All variables are `inline` so each definition is a single instance across
// all translation units that include this header (ODR-safe, C++17 §9.2.6).
// ══════════════════════════════════════════════════════════════════════════════

namespace wl::xdg_decoration {

// ── Forward declarations
// ────────────────────────────────────────────────────── Needed so that types[]
// can take the addresses of the interfaces before their definitions appear.

extern const wl_interface manager_iface;
extern const wl_interface decoration_iface;

// ── Shared pointer array
// ────────────────────────────────────────────────────── Mirrors
// xdg_decoration_unstable_v1_types[] from the C wayland-scanner output.
//
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
inline const wl_interface* types[] = {
    nullptr,                       // [0]  scalar / no-type slots
    &decoration_iface,             // [1]  get_toplevel_decoration → new_id
    &wl::xdg::toplevel_iface,     // [2]  get_toplevel_decoration → toplevel
};

inline constexpr const wl_interface** kScalars = &types[0];
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// ── Message tables
// ────────────────────────────────────────────────────────────

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
inline constexpr wl_message manager_requests[] = {
    {"destroy", "", nullptr},
    {"get_toplevel_decoration", "no", &types[1]},
};

inline constexpr wl_message decoration_requests[] = {
    {"destroy", "", nullptr},
    {"set_mode", "u", kScalars},
    {"unset_mode", "", nullptr},
};

inline constexpr wl_message decoration_events[] = {
    {"configure", "u", kScalars},
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)

// ── Interface object definitions
// ──────────────────────────────────────────────

// clang-format off
inline const wl_interface manager_iface = {
    "zxdg_decoration_manager_v1", 1,
    2, std::data(manager_requests), 0, nullptr};
inline const wl_interface decoration_iface = {
    "zxdg_toplevel_decoration_v1", 1,
    3, std::data(decoration_requests), 1, std::data(decoration_events)};
// clang-format on

}  // namespace wl::xdg_decoration

// ══════════════════════════════════════════════════════════════════════════════
// xdg_decoration_unstable_v1::client traits — wl_iface() inline implementations
// ══════════════════════════════════════════════════════════════════════════════

namespace xdg_decoration_unstable_v1::client {

inline const wl_interface&
zxdg_decoration_manager_v1_traits::wl_iface() noexcept {
  return wl::xdg_decoration::manager_iface;
}
inline const wl_interface&
zxdg_toplevel_decoration_v1_traits::wl_iface() noexcept {
  return wl::xdg_decoration::decoration_iface;
}

}  // namespace xdg_decoration_unstable_v1::client

// ══════════════════════════════════════════════════════════════════════════════
// Generic xdg-decoration CRTP handler templates (namespace wl)
//
// Ready-to-use CRTP handlers for the xdg-decoration-unstable-v1 protocol.
// Include the generated xdg_decoration_unstable_v1_client.hpp before this
// header so that CZxdgDecorationManagerV1<> and CZxdgToplevelDecorationV1<>
// are visible.
//
// Usage example:
//
//   class App {
//     wl::WlPtr<wl::XdgDecorationManagerHandler> decoration_mgr_;
//     wl::WlPtr<wl::XdgDecorationHandler<App>>   decoration_;
//     …
//     void OnDecorationConfigure(uint32_t mode) {
//       use_csd_ = (mode == 1);  // ZxdgToplevelDecorationV1Mode::ClientSide
//     }
//   };
// ══════════════════════════════════════════════════════════════════════════════

namespace wl {

/// XDG decoration manager handler — event-less proxy.
///
/// The manager interface has no events; this handler provides the required
/// ProcessEvent stub so it can be stored in a WlPtr.
class XdgDecorationManagerHandler
    : public xdg_decoration_unstable_v1::client::CZxdgDecorationManagerV1<
          XdgDecorationManagerHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

/// XDG toplevel decoration handler — delegates configure to App.
///
/// Calls app_->OnDecorationConfigure(mode) when the compositor responds with
/// the negotiated decoration mode.
///
/// @tparam App  Application class providing OnDecorationConfigure(uint32_t).
template <typename App>
class XdgDecorationHandler
    : public xdg_decoration_unstable_v1::client::CZxdgToplevelDecorationV1<
          XdgDecorationHandler<App>> {
 public:
  App* app_ = nullptr;

  void OnConfigure(uint32_t mode) override {
    if (app_)
      app_->OnDecorationConfigure(mode);
  }
};

}  // namespace wl
