// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// seat — header-only wl_seat CRTP handler template and self-contained
//        SeatManager that encapsulates the full seat/keyboard lifecycle
//        common to every input-capable example.
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated wayland_client.hpp:
//
//   #include "wayland_client.hpp"   // defines CWlSeat, CWlKeyboard, …
//   #include <wl/seat.hpp>          // wl_iface() impls + SeatManager
//
// seat.hpp automatically includes keyboard.hpp which provides the richer
// wl::KeyboardHandler<App> CRTP class (xkbcommon-capable when available).
//
// ── Provided utilities
// ────────────────────────────────────────────────────────
//
// wl_iface() implementations (namespace wayland::client):
//   Inline out-of-line definitions of the pure-virtual wl_iface() methods
//   declared in wl_seat_traits and wl_keyboard_traits (wayland_client.hpp).
//   Including this header replaces the manual definitions that every example
//   previously duplicated in its .cpp file.
//
// wl::SeatManager<App>:
//   Bundles wl_seat + wl::KeyboardHandler<App> proxy ownership,
//   capability-change handling, and versioned protocol teardown.
//   The App class needs only:
//
//     void OnKey(uint32_t key, uint32_t state);
//
//   Optionally (when WL_HAS_XKBCOMMON is defined):
//
//     void OnKeySym(xkb_keysym_t sym, uint32_t key, uint32_t state);
//
//   Typical usage:
//
//     // member:
//     wl::SeatManager<App> seat_;
//
//     // ScanGlobals lambda:
//     if (iface == wl_seat_traits::interface_name) seat_.Record(name, ver);
//
//     // BindGlobals (no-op if seat not advertised):
//     seat_.Bind(registry_, this);
//
//     // App::~App (before RAII member dtors run):
//     seat_.Release();
#pragma once

#include <wl/client_helpers.hpp>
#include <wl/keyboard.hpp>
#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-protocol.h>
}

#include <algorithm>  // std::min
#include <cstdint>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() implementations — wl_seat and wl_keyboard
//
// <wayland-client-protocol.h> provides pre-built extern const wl_interface
// symbols for every core Wayland interface.  We supply the inline out-of-line
// definitions that the generated traits structs expect.  Making them `inline`
// guarantees a single definition across all TUs (ODR-safe, C++17 §9.2.6).
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

inline const wl_interface& wl_seat_traits::wl_iface() noexcept {
  return wl_seat_interface;
}
inline const wl_interface& wl_keyboard_traits::wl_iface() noexcept {
  return wl_keyboard_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// wl::SeatManager<App>
//
// Self-contained seat + keyboard manager.
//
// Lifecycle:
//   1. Record(name, ver)  — called from the OnGlobal registry callback when
//                           wl_seat is advertised.  Stores the global id and
//                           version for later binding.  No-op if the compositor
//                           does not advertise wl_seat.
//   2. Bind(registry, app) — called after the registry scan roundtrip.
//                            Binds wl_seat and installs the event handler.
//                            No-op (returns true) if Record() was never called.
//   3. Compositor sends wl_seat::capabilities → OnCapabilities is called
//                            internally and creates or releases the keyboard.
//   4. App::~App calls Release() before member destructors run so that the
//                            versioned release requests are sent to the
//                            compositor in the right order.
//
// Destruction order inside SeatManager:
//   keyboard_ is declared AFTER seat_, so C++ destroys keyboard_ FIRST and
//   seat_ SECOND — exactly the correct protocol teardown order.
// ══════════════════════════════════════════════════════════════════════════════

namespace wl {

template <typename App>
class SeatManager {
 public:
  SeatManager() noexcept = default;

  // Non-copyable, non-movable (owns wl_proxy* resources).
  SeatManager(const SeatManager&) = delete;
  SeatManager& operator=(const SeatManager&) = delete;
  SeatManager(SeatManager&&) = delete;
  SeatManager& operator=(SeatManager&&) = delete;

  ~SeatManager() noexcept {
    // Safety net: Release() should have been called explicitly from App::~App
    // before member destructors fire, but guard here in case it was not.
    Release();
  }

  /// Record the wl_seat global id and version for later binding.
  /// Call from the registry OnGlobal callback.
  void Record(uint32_t name, uint32_t ver) noexcept {
    name_ = name;
    ver_adv_ = ver;
  }

  /// Bind wl_seat and install the capabilities event handler.
  ///
  /// No-op (returns true) when Record() was never called — seat is optional.
  /// Returns false only when the seat global was advertised but the bind call
  /// failed (compositor or memory error).
  ///
  /// @param registry  The active registry wrapper.
  /// @param app       Back-pointer forwarded to keyboard's OnKey callback.
  [[nodiscard]] bool Bind(wl::CRegistry& registry, App* app) noexcept {
    if (!name_)
      return true;  // seat not advertised — optional, not an error
    using namespace wayland::client;
    ver_ = std::min(ver_adv_, wl_seat_traits::version);
    app_ = app;
    if (!wl::BindHandler<wl_seat_traits>(registry, seat_, name_, ver_))
      return false;
    seat_.Get()->mgr_ = this;
    return true;
  }

  /// Send versioned release requests and destroy proxies.
  ///
  /// Must be called from App::~App BEFORE member destructors run so that
  /// the protocol messages can still be flushed over the display connection.
  void Release() noexcept {
    ReleaseKeyboard();
    ReleaseSeat();
  }

  /// Returns the read end of the self-pipe used for key-repeat signaling.
  ///
  /// Returns -1 when no keyboard is bound or when repeat setup failed.
  /// Pass this fd to RunEventLoop (or your own poll loop) alongside the
  /// Wayland display fd; call DispatchRepeat() each time it is readable.
  [[nodiscard]] int GetRepeatFd() const noexcept {
    if (keyboard_.IsNull())
      return -1;
    return keyboard_.Get()->GetRepeatFd();
  }

  /// Fire one key-repeat event.
  ///
  /// Must be called from the main event-loop thread.  Drains the self-pipe
  /// and calls OnKeySym / OnKey on the App for the currently repeating key.
  /// No-op when no keyboard is bound.
  void DispatchRepeat() noexcept {
    if (!keyboard_.IsNull())
      keyboard_.Get()->DispatchRepeat();
  }

  /// Internal capability handler — called by the seat proxy on capabilities
  /// change.  Creates the keyboard when keyboard capability is gained; releases
  /// it when lost.
  void OnCapabilities(uint32_t caps) noexcept {
    using namespace wayland::client;
    const bool has_kbd = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0u;
    if (has_kbd && keyboard_.IsNull()) {
      if (wl::SetupHandler(
              keyboard_,
              wl::construct<wl_keyboard_traits,
                            wl_seat_traits::Op::GetKeyboard>(*seat_.Get()))) {
        keyboard_.Get()->app_ = app_;
      }
    } else if (!has_kbd && !keyboard_.IsNull()) {
      ReleaseKeyboard();
    }
  }

 private:
  // ── Internal seat handler ──────────────────────────────────────────────────
  // Delegates OnCapabilities to the owning SeatManager so that keyboard
  // creation/release is handled internally without App involvement.
  struct SeatHandler : public wayland::client::CWlSeat<SeatHandler> {
    SeatManager* mgr_ = nullptr;
    void OnCapabilities(uint32_t caps) override { mgr_->OnCapabilities(caps); }
    void OnName(const char* /*name*/) override {}
  };

  // ── Members ────────────────────────────────────────────────────────────────
  // Declaration order = reverse destruction order.
  // seat_ declared before keyboard_ → seat_ is destroyed AFTER keyboard_.
  wl::WlPtr<SeatHandler> seat_;
  wl::WlPtr<wl::KeyboardHandler<App>> keyboard_;

  uint32_t name_ = 0;     // global id recorded during ScanGlobals
  uint32_t ver_adv_ = 0;  // advertised version
  uint32_t ver_ = 0;      // negotiated (clamped) version used for teardown
  App* app_ = nullptr;

  // ── Teardown helpers ───────────────────────────────────────────────────────
  void ReleaseKeyboard() noexcept {
    if (keyboard_.IsNull())
      return;
    using Kbd = wayland::client::wl_keyboard_traits;
    if (ver_ >= Kbd::Op::Since::Release)
      keyboard_.Get()->Release();
    keyboard_.Reset();
  }

  void ReleaseSeat() noexcept {
    if (seat_.IsNull())
      return;
    using S = wayland::client::wl_seat_traits;
    if (ver_ >= S::Op::Since::Release)
      seat_.Get()->Release();
    seat_.Reset();
  }
};

}  // namespace wl
