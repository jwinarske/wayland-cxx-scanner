// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// pointer — header-only wl_pointer CRTP handler that converts surface-local
// coordinates and forwards typed events to optional App hooks.  It is a peer of
// wl::KeyboardHandler and is created/destroyed by wl::SeatManager on the seat's
// pointer capability, so consumers never touch wl_pointer directly.
//
// This header must be included AFTER the generated wayland_client.hpp:
//   #include "wayland_client.hpp"   // defines CWlPointer, wl_pointer_traits
//   #include <wl/pointer.hpp>       // wl::PointerHandler<App>
//
// The App implements whichever hooks it needs; every one is optional and
// detected via SFINAE, so a consumer that wants only some of them (or none)
// compiles unchanged:
//
//   void OnPointerEnter(const wl::PointerEvent& ev);
//   void OnPointerLeave();
//   void OnPointerMotion(const wl::PointerEvent& ev);
//   void OnPointerButton(const wl::PointerButtonEvent& ev);
//
// SeatManager only binds the pointer when the App defines at least one hook, so
// keyboard-only consumers are entirely unaffected.

#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

extern "C" {
#include <wayland-util.h>  // wl_fixed_t, wl_fixed_to_double
}

namespace wl {

// A pointer position in surface-local logical pixels.  `serial` is the enter
// serial (0 for motion); `time` is the event timestamp (0 for enter).
struct PointerEvent {
  double x = 0.0;
  double y = 0.0;
  std::uint32_t serial = 0;
  std::uint32_t time = 0;
};

// A pointer button event, carrying the last known position so consumers do not
// have to track it themselves.
struct PointerButtonEvent {
  double x = 0.0;
  double y = 0.0;
  std::uint32_t serial = 0;
  std::uint32_t time = 0;
  std::uint32_t button = 0;  // BTN_LEFT, BTN_RIGHT, …
  std::uint32_t state = 0;   // WL_POINTER_BUTTON_STATE_PRESSED / _RELEASED
};

// ══════════════════════════════════════════════════════════════════════════════
// wl::PointerHandler<App>
//
// Tracks the latest pointer position and forwards enter/leave/motion/button to
// the App's optional hooks.  Axis (scroll) and frame events use the generated
// no-op defaults.
// ══════════════════════════════════════════════════════════════════════════════

template <typename App>
class PointerHandler : public wayland::client::CWlPointer<PointerHandler<App>> {
 public:
  /// Back-pointer set by SeatManager immediately after SetupHandler() returns.
  App* app_ = nullptr;

  void OnEnter(std::uint32_t serial,
               wl_proxy* /*surface*/,
               wl_fixed_t sx,
               wl_fixed_t sy) override {
    x_ = wl_fixed_to_double(sx);
    y_ = wl_fixed_to_double(sy);
    if (app_ != nullptr)
      CallEnter(PointerEvent{x_, y_, serial, 0u});
  }

  void OnLeave(std::uint32_t /*serial*/, wl_proxy* /*surface*/) override {
    if (app_ != nullptr)
      CallLeave(0);
  }

  void OnMotion(std::uint32_t time, wl_fixed_t sx, wl_fixed_t sy) override {
    x_ = wl_fixed_to_double(sx);
    y_ = wl_fixed_to_double(sy);
    if (app_ != nullptr)
      CallMotion(PointerEvent{x_, y_, 0u, time});
  }

  void OnButton(std::uint32_t serial,
                std::uint32_t time,
                std::uint32_t button,
                std::uint32_t state) override {
    if (app_ != nullptr)
      CallButton(PointerButtonEvent{x_, y_, serial, time, button, state});
  }

 private:
  double x_ = 0.0;
  double y_ = 0.0;

  // ── Optional App hooks (detected via SFINAE, mirroring KeyboardHandler)
  // ─────
  template <typename A = App>
  auto CallEnter(const PointerEvent& e)
      -> decltype(std::declval<A&>().OnPointerEnter(e), void()) {
    app_->OnPointerEnter(e);
  }
  void CallEnter(...) noexcept {}

  template <typename A = App>
  auto CallLeave(int) -> decltype(std::declval<A&>().OnPointerLeave(), void()) {
    app_->OnPointerLeave();
  }
  void CallLeave(...) noexcept {}

  template <typename A = App>
  auto CallMotion(const PointerEvent& e)
      -> decltype(std::declval<A&>().OnPointerMotion(e), void()) {
    app_->OnPointerMotion(e);
  }
  void CallMotion(...) noexcept {}

  template <typename A = App>
  auto CallButton(const PointerButtonEvent& e)
      -> decltype(std::declval<A&>().OnPointerButton(e), void()) {
    app_->OnPointerButton(e);
  }
  void CallButton(...) noexcept {}
};

namespace detail {

// True when App defines any pointer hook.  SeatManager creates the wl_pointer
// only then, so keyboard-only consumers never allocate one.
template <typename A, typename = void>
struct HasPointerButton : std::false_type {};
template <typename A>
struct HasPointerButton<A,
                        std::void_t<decltype(std::declval<A&>().OnPointerButton(
                            std::declval<const PointerButtonEvent&>()))>>
    : std::true_type {};

template <typename A, typename = void>
struct HasPointerMotion : std::false_type {};
template <typename A>
struct HasPointerMotion<A,
                        std::void_t<decltype(std::declval<A&>().OnPointerMotion(
                            std::declval<const PointerEvent&>()))>>
    : std::true_type {};

template <typename A, typename = void>
struct HasPointerEnter : std::false_type {};
template <typename A>
struct HasPointerEnter<A,
                       std::void_t<decltype(std::declval<A&>().OnPointerEnter(
                           std::declval<const PointerEvent&>()))>>
    : std::true_type {};

template <typename A>
inline constexpr bool WantsPointer =
    HasPointerButton<A>::value || HasPointerMotion<A>::value ||
    HasPointerEnter<A>::value;

}  // namespace detail

}  // namespace wl
