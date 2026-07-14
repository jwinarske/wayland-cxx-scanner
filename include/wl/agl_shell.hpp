// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// agl-shell — generic CRTP handler template for the AGL shell protocol
// (agl-shell.xml).
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated agl_shell_client.hpp:
//
//   #include "agl_shell_client.hpp"  // defines CAglShell, agl_shell_traits
//   #include <wl/agl_shell.hpp>      // wl::AglShellHandler<App>
//
// agl_shell is not part of wayland-protocols, so unlike wl_compositor there is
// no pre-built wl_interface symbol to link against.  Generate the client header
// with --emit-interface-tables and it carries its own tables and wl_iface()
// definition, derived from the XML.  This header once hand-wrote them, which
// pinned the supported protocol version to whatever the tables spelled out; the
// scanner reads the version from the XML instead.
//
// ── Provided utilities
// ────────────────────────────────────────────────────────
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

#include <cstdint>

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
