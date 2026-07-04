// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// touch — header-only wl_touch CRTP handler that tracks multiple concurrent
// contacts and forwards typed events to optional App hooks.  It is a peer of
// wl::KeyboardHandler / wl::PointerHandler and is created/destroyed by
// wl::SeatManager on the seat's touch capability.
//
// Multi-touch is first-class: up to kMaxPoints contacts are tracked in a
// compacted, fixed-capacity array (no per-event allocation), sized for at least
// a 10-point touchscreen.  The full active set is delivered on wl_touch.frame,
// the natural boundary at which to render or hit-test every finger at once.
//
// This header must be included AFTER the generated wayland_client.hpp:
//   #include "wayland_client.hpp"   // defines CWlTouch, wl_touch_traits
//   #include <wl/touch.hpp>         // wl::TouchHandler<App>
//
// The App implements whichever hooks it needs; every one is optional and
// detected via SFINAE:
//   void OnTouchDown(const wl::TouchPoint& p);
//   void OnTouchMotion(const wl::TouchPoint& p);
//   void OnTouchUp(int32_t id);
//   void OnTouchFrame(wl::span<const wl::TouchPoint> points);  // active set
//   void OnTouchCancel();
//
// SeatManager only binds the touch device when the App defines at least one
// hook, so consumers without touch are entirely unaffected.

#pragma once

#include <wl/span.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

extern "C" {
#include <wayland-util.h>  // wl_fixed_t, wl_fixed_to_double
}

namespace wl {

// A single touch contact in surface-local logical pixels.  `id` is the
// wl_touch contact id assigned by the compositor for the lifetime of the touch.
struct TouchPoint {
  std::int32_t id = -1;
  double x = 0.0;
  double y = 0.0;
};

// ══════════════════════════════════════════════════════════════════════════════
// wl::TouchHandler<App>
// ══════════════════════════════════════════════════════════════════════════════

template <typename App>
class TouchHandler : public wayland::client::CWlTouch<TouchHandler<App>> {
 public:
  // Concurrent contacts tracked without allocation.  Sized for a 10-point
  // controller; raise for hardware that reports more.  A contact beyond the cap
  // is ignored (never partially tracked), so existing points stay smooth.
  static constexpr std::size_t kMaxPoints = 10;

  /// Back-pointer set by SeatManager immediately after SetupHandler() returns.
  App* app_ = nullptr;

  void OnDown(std::uint32_t /*serial*/,
              std::uint32_t /*time*/,
              wl_proxy* /*surface*/,
              std::int32_t id,
              wl_fixed_t x,
              wl_fixed_t y) override {
    TouchPoint* p = Add(id, wl_fixed_to_double(x), wl_fixed_to_double(y));
    if (p != nullptr && app_ != nullptr)
      CallDown(*p);
  }

  void OnUp(std::uint32_t /*serial*/,
            std::uint32_t /*time*/,
            std::int32_t id) override {
    if (Remove(id) && app_ != nullptr)
      CallUp(id);
  }

  void OnMotion(std::uint32_t /*time*/,
                std::int32_t id,
                wl_fixed_t x,
                wl_fixed_t y) override {
    TouchPoint* p = Find(id);
    if (p == nullptr)
      return;
    p->x = wl_fixed_to_double(x);
    p->y = wl_fixed_to_double(y);
    if (app_ != nullptr)
      CallMotion(*p);
  }

  // A frame batches all down/up/motion since the previous frame; deliver the
  // full active set so the App can update every contact atomically.
  void OnFrame() override {
    if (app_ != nullptr)
      CallFrame(active());
  }

  // The compositor aborted the gesture (e.g. it became a system gesture); drop
  // every contact.
  void OnCancel() override {
    count_ = 0;
    if (app_ != nullptr)
      CallCancel(0);
  }

  // The current active contacts, compacted (no gaps).
  [[nodiscard]] span<const TouchPoint> active() const noexcept {
    return {points_.data(), count_};
  }

 private:
  std::array<TouchPoint, kMaxPoints> points_{};
  std::size_t count_ = 0;

  TouchPoint* Find(std::int32_t id) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (points_.at(i).id == id)
        return &points_.at(i);
    }
    return nullptr;
  }

  TouchPoint* Add(std::int32_t id, double x, double y) noexcept {
    if (TouchPoint* existing = Find(id)) {  // duplicate id — refresh in place
      existing->x = x;
      existing->y = y;
      return existing;
    }
    if (count_ >= kMaxPoints)
      return nullptr;
    points_.at(count_) = TouchPoint{id, x, y};
    return &points_.at(count_++);
  }

  bool Remove(std::int32_t id) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (points_.at(i).id == id) {
        // Swap-remove keeps [0, count_) compact so active() has no gaps.
        points_.at(i) = points_.at(--count_);
        return true;
      }
    }
    return false;
  }

  // ── Optional App hooks (detected via SFINAE)
  // ────────────────────────────────
  template <typename A = App>
  auto CallDown(const TouchPoint& p)
      -> decltype(std::declval<A&>().OnTouchDown(p), void()) {
    app_->OnTouchDown(p);
  }
  void CallDown(...) noexcept {}

  template <typename A = App>
  auto CallUp(std::int32_t id)
      -> decltype(std::declval<A&>().OnTouchUp(id), void()) {
    app_->OnTouchUp(id);
  }
  void CallUp(...) noexcept {}

  template <typename A = App>
  auto CallMotion(const TouchPoint& p)
      -> decltype(std::declval<A&>().OnTouchMotion(p), void()) {
    app_->OnTouchMotion(p);
  }
  void CallMotion(...) noexcept {}

  template <typename A = App>
  auto CallFrame(span<const TouchPoint> pts)
      -> decltype(std::declval<A&>().OnTouchFrame(pts), void()) {
    app_->OnTouchFrame(pts);
  }
  void CallFrame(...) noexcept {}

  template <typename A = App>
  auto CallCancel(int) -> decltype(std::declval<A&>().OnTouchCancel(), void()) {
    app_->OnTouchCancel();
  }
  void CallCancel(...) noexcept {}
};

namespace detail {

// True when App defines any touch hook.  SeatManager binds the touch device
// only then.
template <typename A, typename = void>
struct HasTouchDown : std::false_type {};
template <typename A>
struct HasTouchDown<A,
                    std::void_t<decltype(std::declval<A&>().OnTouchDown(
                        std::declval<const TouchPoint&>()))>> : std::true_type {
};

template <typename A, typename = void>
struct HasTouchMotion : std::false_type {};
template <typename A>
struct HasTouchMotion<A,
                      std::void_t<decltype(std::declval<A&>().OnTouchMotion(
                          std::declval<const TouchPoint&>()))>>
    : std::true_type {};

template <typename A, typename = void>
struct HasTouchFrame : std::false_type {};
template <typename A>
struct HasTouchFrame<A,
                     std::void_t<decltype(std::declval<A&>().OnTouchFrame(
                         std::declval<span<const TouchPoint>>()))>>
    : std::true_type {};

template <typename A>
inline constexpr bool WantsTouch =
    HasTouchDown<A>::value || HasTouchMotion<A>::value ||
    HasTouchFrame<A>::value;

}  // namespace detail

}  // namespace wl
