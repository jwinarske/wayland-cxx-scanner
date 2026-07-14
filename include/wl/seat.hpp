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
//   declared in wl_seat_traits, wl_keyboard_traits, wl_pointer_traits, and
//   wl_touch_traits (wayland_client.hpp).  Including this header replaces the
//   manual definitions that every example previously duplicated in its .cpp
//   file.
//
// wl::SeatManager<App>:
//   Bundles wl_seat + wl::KeyboardHandler<App> proxy ownership,
//   capability-change handling, and versioned protocol teardown.
//   The App class needs only:
//
//     void OnKey(const wl::KeyEvent& ev);
//
//   Optionally:
//
//     void OnKeymap(xkb_keymap* keymap);
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
#include <wl/pointer.hpp>
#include <wl/touch.hpp>
#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-protocol.h>
}

#include <algorithm>  // std::min
#include <cstdint>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() implementations — every interface SeatManager binds
//
// <wayland-client-protocol.h> provides pre-built extern const wl_interface
// symbols for every core Wayland interface.  We supply the inline out-of-line
// definitions that the generated traits structs expect.  Making them `inline`
// guarantees a single definition across all TUs (ODR-safe, C++17 §9.2.6).
//
// SeatManager creates the pointer and touch proxies itself, on a capability
// change the App never sees, so their tables belong here next to the keyboard's
// rather than in each App that happens to define a pointer or touch hook.
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

inline const wl_interface& wl_seat_traits::wl_iface() noexcept {
  return wl_seat_interface;
}
inline const wl_interface& wl_keyboard_traits::wl_iface() noexcept {
  return wl_keyboard_interface;
}
inline const wl_interface& wl_pointer_traits::wl_iface() noexcept {
  return wl_pointer_interface;
}
inline const wl_interface& wl_touch_traits::wl_iface() noexcept {
  return wl_touch_interface;
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
    ReleaseTouch();
    ReleasePointer();
    ReleaseKeyboard();
    ReleaseSeat();
  }

  /// The bound wl_seat proxy, or nullptr when no seat is bound.
  ///
  /// The seat is bound once and lifetime-stable, so this handle is safe to pass
  /// to requests that take a seat — e.g. xdg_toplevel.move / .resize with a
  /// pointer button serial (see wl::PointerButtonEvent::serial).
  [[nodiscard]] wl_proxy* Seat() const noexcept {
    return seat_.IsNull() ? nullptr : seat_.Get()->GetProxy();
  }

  /// The bound wl_pointer proxy, or nullptr when no pointer is bound (the App
  /// defines no pointer hook, or the seat has no pointer capability).
  ///
  /// Exposed so a consumer can attach a cursor with wl_pointer.set_cursor (see
  /// wl::CursorManager), which SeatManager does not do itself — the pointer's
  /// lifetime is managed here, but the cursor surface, its wl_shm buffers, and
  /// the theme belong to the consumer.
  [[nodiscard]] wl_proxy* Pointer() const noexcept {
    return pointer_.IsNull() ? nullptr : pointer_.Get()->GetProxy();
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

    // The pointer is created only when the App defines a pointer hook, so
    // keyboard-only consumers never bind one (and never surprise the compositor
    // by holding a cursor-less pointer).
    if constexpr (wl::detail::WantsPointer<App>) {
      const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0u;
      if (has_ptr && pointer_.IsNull()) {
        if (wl::SetupHandler(
                pointer_,
                wl::construct<wl_pointer_traits,
                              wl_seat_traits::Op::GetPointer>(*seat_.Get()))) {
          pointer_.Get()->app_ = app_;
        }
      } else if (!has_ptr && !pointer_.IsNull()) {
        ReleasePointer();
      }
    }

    // Touch is likewise bound only when the App defines a touch hook.
    if constexpr (wl::detail::WantsTouch<App>) {
      const bool has_touch = (caps & WL_SEAT_CAPABILITY_TOUCH) != 0u;
      if (has_touch && touch_.IsNull()) {
        if (wl::SetupHandler(
                touch_,
                wl::construct<wl_touch_traits, wl_seat_traits::Op::GetTouch>(
                    *seat_.Get()))) {
          touch_.Get()->app_ = app_;
        }
      } else if (!has_touch && !touch_.IsNull()) {
        ReleaseTouch();
      }
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
  wl::WlPtr<wl::PointerHandler<App>> pointer_;
  wl::WlPtr<wl::TouchHandler<App>> touch_;

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

  void ReleasePointer() noexcept {
    if (pointer_.IsNull())
      return;
    using Ptr = wayland::client::wl_pointer_traits;
    if (ver_ >= Ptr::Op::Since::Release)
      pointer_.Get()->Release();
    pointer_.Reset();
  }

  void ReleaseTouch() noexcept {
    if (touch_.IsNull())
      return;
    using Tch = wayland::client::wl_touch_traits;
    if (ver_ >= Tch::Op::Since::Release)
      touch_.Get()->Release();
    touch_.Reset();
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
