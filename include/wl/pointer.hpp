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
//   void OnPointerAxis(const wl::PointerAxisEvent& ev);
//   void OnPointerFrame();
//
// SeatManager only binds the pointer when the App defines at least one hook, so
// keyboard-only consumers are entirely unaffected.
//
// ── Scroll ───────────────────────────────────────────────────────────────────
//
// wl_pointer reports scrolling several different ways depending on the
// negotiated version and the device behind it.  The handler hides that spread
// and hands the App one PointerAxisEvent per scrolled axis per frame, carrying
// both a normalized value120 (120 == one wheel notch) and the raw continuous
// distance.  value120 is resolved by taking the first of these that applies:
//
//   1. axis_value120     → used verbatim.  High-resolution wheels on version 8
//                          and up; the only source of sub-notch precision.
//   2. axis_discrete     → discrete × 120.  Wheels on versions 5–7, and any
//                          version 8 compositor that omits value120: an exact
//                          notch count still beats estimating from distance.
//   3. otherwise         → synthesized from the raw distance: axis / 10 × 120,
//                          libinput's 10-units-per-notch convention.  This
//                          covers continuous sources (finger, continuous),
//                          which send no discrete steps at any version, and
//                          pre-version-5 seats, which have no discrete event.
//
// Version 5 also introduced wl_pointer.frame.  Before it there is no batch
// boundary to accumulate against, so each axis event is flushed as it arrives
// and forms its own implicit frame.
//
// `continuous` is always the raw accumulated distance, so a consumer that wants
// smooth kinetic scrolling can ignore value120 entirely.  axis_stop sets
// `stop`; it is a flag on the frame's event rather than a value of its own, so
// a lone axis_stop reports value120 == 0 while a frame carrying both a final
// axis and its stop still reports that last distance.
//
// Accumulation is per axis and resets on frame and on leave.  OnPointerAxis
// fires before OnPointerFrame within a frame, so an App that batches can read
// the axis events it just received and act once on the frame boundary.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

extern "C" {
#include <wayland-client-core.h>  // wl_proxy_get_version
#include <wayland-util.h>         // wl_fixed_t, wl_fixed_to_double
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

// Stands in for the scroll source whenever the compositor does not name one.
// wl_pointer.axis_source does not exist before version 5, and is optional at
// every version after it, so an App must always handle an unknown source.
inline constexpr std::uint32_t kAxisSourceUnknown = 0xffffffffu;

// One axis of scrolling, accumulated over a frame and normalized.
struct PointerAxisEvent {
  // WlPointerAxis: VerticalScroll (0) / HorizontalScroll (1).
  std::uint32_t axis = 0;
  // Notch-normalized scroll amount; 120 is one detent of a classic wheel.
  // Synthesized for continuous devices, so it is not always a multiple of 120.
  std::int32_t value120 = 0;
  // Raw accumulated scroll distance in surface-local logical pixels — what a
  // smooth-scrolling consumer wants for source = finger / continuous.
  double continuous = 0.0;
  // WlPointerAxisSource, or kAxisSourceUnknown when the compositor omitted
  // axis_source — which it may do at any version, not only before version 5.
  std::uint32_t source = kAxisSourceUnknown;
  std::uint32_t time = 0;
  // The compositor ended a kinetic scroll on this axis (wl_pointer.axis_stop).
  // Consumers that do not model momentum can ignore it; it does not suppress
  // value120 / continuous, so a final axis event in the same frame still
  // counts.
  bool stop = false;
};

// ══════════════════════════════════════════════════════════════════════════════
// wl::PointerHandler<App>
//
// Tracks the latest pointer position and forwards enter/leave/motion/button and
// normalized scroll to the App's optional hooks.  See the scroll table at the
// top of this header for the version-spanning axis contract.
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
    // A frame may never arrive for whatever was accumulated before the pointer
    // left, so drop it rather than misattribute it to the next surface.
    ResetAxes();
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

  // ── Axis (scroll) ─────────────────────────────────────────────────────────
  // Every axis event accumulates into per-axis state; the frame boundary
  // normalizes and delivers it.

  void OnAxis(std::uint32_t time,
              std::uint32_t axis,
              wl_fixed_t value) override {
    Axis* a = AxisFor(axis);
    if (a == nullptr)
      return;
    a->active = true;
    a->time = time;
    a->continuous += wl_fixed_to_double(value);
    // Before version 5 there is no frame event to batch on, so each axis event
    // is its own implicit frame.
    if (Version() < 5u)
      Flush();
  }

  void OnAxisSource(std::uint32_t axis_source) override {
    source_ = axis_source;
  }

  void OnAxisStop(std::uint32_t time, std::uint32_t axis) override {
    Axis* a = AxisFor(axis);
    if (a == nullptr)
      return;
    a->active = true;
    a->stop = true;
    a->time = time;
  }

  void OnAxisDiscrete(std::uint32_t axis, std::int32_t discrete) override {
    Axis* a = AxisFor(axis);
    if (a == nullptr)
      return;
    a->active = true;
    a->has_discrete = true;
    a->discrete += discrete;
  }

  void OnAxisValue120(std::uint32_t axis, std::int32_t value120) override {
    Axis* a = AxisFor(axis);
    if (a == nullptr)
      return;
    a->active = true;
    a->has_value120 = true;
    a->value120 += value120;
  }

  void OnFrame() override { Flush(); }

 private:
  double x_ = 0.0;
  double y_ = 0.0;

  // ── Axis accumulation ─────────────────────────────────────────────────────
  // wl_pointer has exactly two axes; index == WlPointerAxis.
  static constexpr std::size_t kAxisCount = 2;

  static constexpr std::int32_t kValue120Min =
      std::numeric_limits<std::int32_t>::min();
  static constexpr std::int32_t kValue120Max =
      std::numeric_limits<std::int32_t>::max();

  struct Axis {
    bool active = false;  // any axis event landed on this axis this frame
    double continuous = 0.0;
    // Accumulated in 64 bits: the compositor controls both the per-event
    // magnitude and how many events it packs into one frame, so summing the
    // wire values at their native width could overflow (UB) on bad input.
    // Flush() clamps back down to the event's int32.
    std::int64_t value120 = 0;
    std::int64_t discrete = 0;
    bool has_value120 = false;
    bool has_discrete = false;
    bool stop = false;
    std::uint32_t time = 0;
  };

  std::array<Axis, kAxisCount> axes_{};
  std::uint32_t source_ = kAxisSourceUnknown;

  Axis* AxisFor(std::uint32_t axis) noexcept {
    return axis < kAxisCount ? &axes_.at(axis) : nullptr;
  }

  /// The negotiated wl_pointer version, straight from the proxy — get_pointer
  /// inherits it from the seat, so the proxy is the authority and there is
  /// nothing to cache.  A handler with no proxy is a unit test or a bug;
  /// assume the newest, which keeps value120 the default.
  [[nodiscard]] std::uint32_t Version() const noexcept {
    wl_proxy* const proxy = this->GetProxy();
    if (proxy != nullptr)
      return wl_proxy_get_version(proxy);
    return wayland::client::wl_pointer_traits::version;
  }

  /// Saturate an accumulated notch count into the event's int32.  Clamping
  /// rather than wrapping keeps a nonsensical compositor value merely huge
  /// instead of flipping the scroll direction.
  static std::int32_t ClampValue120(std::int64_t v) noexcept {
    constexpr auto kLo = static_cast<std::int64_t>(kValue120Min);
    constexpr auto kHi = static_cast<std::int64_t>(kValue120Max);
    return static_cast<std::int32_t>(v < kLo ? kLo : (v > kHi ? kHi : v));
  }

  /// libinput reports one wheel detent as 10 units of continuous scroll.
  /// Clamped before the narrowing conversion, which would otherwise be
  /// undefined for a distance outside int32's range.
  static std::int32_t SynthesizeValue120(double continuous) noexcept {
    const double notches = continuous / 10.0 * 120.0;
    if (notches <= static_cast<double>(kValue120Min))
      return kValue120Min;
    if (notches >= static_cast<double>(kValue120Max))
      return kValue120Max;
    return static_cast<std::int32_t>(std::lround(notches));
  }

  /// Normalize and deliver every axis touched since the last frame, then hand
  /// the App the frame boundary itself.
  void Flush() {
    const std::uint32_t version = Version();
    for (std::size_t i = 0; i < kAxisCount; ++i) {
      Axis& a = axes_.at(i);
      if (!a.active)
        continue;
      PointerAxisEvent ev{};
      ev.axis = static_cast<std::uint32_t>(i);
      ev.continuous = a.continuous;
      ev.source = source_;
      ev.time = a.time;
      ev.stop = a.stop;
      // The ladder documented at the top of this header, in order.  `stop` is
      // deliberately not a rung: a lone axis_stop accumulates no distance and
      // so synthesizes to 0 on its own, while a frame carrying both a final
      // axis and its stop still reports that last movement.
      if (version >= 8u && a.has_value120) {
        ev.value120 =
            ClampValue120(a.value120);  // authoritative; discrete is redundant
      } else if (a.has_discrete) {
        ev.value120 = ClampValue120(a.discrete * 120);
      } else {
        ev.value120 = SynthesizeValue120(a.continuous);
      }
      if (app_ != nullptr)
        CallAxis(ev);
    }
    ResetAxes();
    if (app_ != nullptr)
      CallFrame(0);
  }

  void ResetAxes() noexcept {
    axes_ = {};
    source_ = kAxisSourceUnknown;
  }

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

  template <typename A = App>
  auto CallAxis(const PointerAxisEvent& e)
      -> decltype(std::declval<A&>().OnPointerAxis(e), void()) {
    app_->OnPointerAxis(e);
  }
  void CallAxis(...) noexcept {}

  template <typename A = App>
  auto CallFrame(int) -> decltype(std::declval<A&>().OnPointerFrame(), void()) {
    app_->OnPointerFrame();
  }
  void CallFrame(...) noexcept {}
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

template <typename A, typename = void>
struct HasPointerAxis : std::false_type {};
template <typename A>
struct HasPointerAxis<A,
                      std::void_t<decltype(std::declval<A&>().OnPointerAxis(
                          std::declval<const PointerAxisEvent&>()))>>
    : std::true_type {};

// OnPointerFrame counts on its own: an App that only wants the batch boundary
// (to coalesce a redraw, say) is a legitimate consumer, and leaving it out here
// would bind no pointer and strand the hook, silently and with no diagnostic.
template <typename A, typename = void>
struct HasPointerFrame : std::false_type {};
template <typename A>
struct HasPointerFrame<
    A,
    std::void_t<decltype(std::declval<A&>().OnPointerFrame())>>
    : std::true_type {};

template <typename A>
inline constexpr bool WantsPointer =
    HasPointerButton<A>::value || HasPointerMotion<A>::value ||
    HasPointerEnter<A>::value || HasPointerAxis<A>::value ||
    HasPointerFrame<A>::value;

}  // namespace detail

}  // namespace wl
