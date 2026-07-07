// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// keyboard — header-only CRTP wl_keyboard handler with xkbcommon keymap
//             processing and keyboard repeat.
//
// libxkbcommon is assumed to be universally available on any system that
// runs a Wayland compositor; it is treated as a required dependency.
//
// ── Include order ────────────────────────────────────────────────────────────
// This header must be included AFTER the generated wayland_client.hpp:
//
//   #include "wayland_client.hpp"   // defines CWlKeyboard
//   #include <wl/keyboard.hpp>      // wl::KeyboardHandler<App>
//
// ── App contract ─────────────────────────────────────────────────────────────
// The App class must expose a single key-event sink:
//
//   void OnKey(const wl::KeyEvent& ev);
//
// KeyEvent carries everything the handler resolved for one event: the Linux
// evdev keycode, the translated XKB keysym (XKB_KEY_NoSymbol when no keymap is
// loaded), the press/release state, the effective modifier mask, and whether
// the event was synthesized by key repeat.  This is the only required method;
// the same struct is delivered for real key events and for repeat ticks, so an
// App needs exactly one code path.
//
// App may additionally provide (detected via SFINAE):
//
//   void OnKeymap(xkb_keymap* keymap);
//
// called once each time a new compositor keymap is compiled — useful for
// extracting modifier indices (e.g. Ctrl / Alt mask bits) up front.
//
// ── Keyboard repeat ──────────────────────────────────────────────────────────
// Keyboard repeat is driven by a timerfd (CLOCK_MONOTONIC), not a POSIX
// interval timer + signal:
//
//   • A single timerfd is created per handler in the constructor.  It carries
//     no process-global state (unlike a SIGRTMIN signal handler), uses a
//     monotonic clock (immune to wall-clock / NTP steps), and is directly
//     pollable, so it composes into any poll/epoll/reactor the consumer runs.
//
//   • OnRepeatInfo: stores the compositor-advertised rate / delay.  A rate of
//     zero disables repeat (per the Wayland protocol).
//
//   • OnKey: on WL_KEYBOARD_KEY_STATE_PRESSED, arms the timerfd (initial delay
//     then periodic interval derived from the rate) if the key is repeatable
//     (xkb_keymap_key_repeats) and the rate is non-zero.  On
//     WL_KEYBOARD_KEY_STATE_RELEASED for the currently repeating key, disarms.
//
//   • OnLeave: disarms when the keyboard focus leaves the surface, matching the
//     Wayland protocol requirement.
//
//   • GetRepeatFd(): returns the timerfd (-1 if creation failed).  Pass it to
//     RunEventLoop (or your own poll loop) so the loop wakes when a repeat
//     fires.
//
//   • DispatchRepeat(): drains the timerfd and fires one repeat event on app_.
//     Must be called from the main event-loop thread.  The keysym and modifier
//     mask are re-resolved from the current xkb_state at dispatch time, so a
//     Shift / AltGr change held mid-repeat takes effect on the next tick.
//
//   • wl_keyboard v10 server-driven repeat: a compositor may instead advertise
//     a zero rate and deliver key events with state "repeated".  OnKey
//     normalizes those to a pressed KeyEvent with repeat == true, so consumers
//     handle client- and server-driven repeat through one path; the timerfd
//     stays disarmed (rate 0), so the two never double up.
//
// If the timerfd could not be created, is_repeat_valid() returns false; all
// other keyboard events continue to work normally.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
}

#include <cstdint>
#include <utility>

namespace wl {

// ── KeyEvent ─────────────────────────────────────────────────────────────────
// A single keyboard event delivered to App::OnKey.  The same struct is used for
// real key events and for repeat ticks (repeat == true); the keysym and
// modifier mask are XKB_KEY_NoSymbol / 0 when no keymap is loaded.
struct KeyEvent {
  uint32_t key;         ///< Linux evdev keycode (KEY_ESC, KEY_SPACE, …).
  xkb_keysym_t keysym;  ///< Translated XKB keysym, or XKB_KEY_NoSymbol.
  uint32_t state;       ///< WL_KEYBOARD_KEY_STATE_PRESSED / _RELEASED.
  uint32_t modifiers;   ///< Serialized effective xkb modifier mask.
  bool repeat;          ///< True when synthesized by key repeat.
  uint32_t serial;      ///< wl_keyboard.key serial; usable for requests that
                        ///< need an input serial (e.g. set_selection).  For a
                        ///< client-side repeat tick it carries the serial of
                        ///< the originating press.
};

// ══════════════════════════════════════════════════════════════════════════════
// wl::KeyboardHandler<App>
//
// CRTP keyboard handler that wraps wl_keyboard and performs full xkbcommon
// keymap processing with timerfd-driven keyboard repeat.
//
// Key design points:
//   • OnKeymap: mmaps the compositor-provided keymap fd (MAP_PRIVATE for
//     wl_keyboard v7+), compiles it via xkb_keymap_new_from_string, and builds
//     a fresh xkb_state.  The fd is always closed after processing, as required
//     by the Wayland protocol (fd ownership is transferred).  Calls the
//     optional App::OnKeymap(xkb_keymap*) once the new keymap is in place.
//   • OnKey: translates the Linux evdev scancode to an XKB scancode
//     (evdev_code + 8), resolves the keysym + effective modifier mask in the
//     current xkb_state, and delivers a KeyEvent to app_->OnKey.  Arms /
//     disarms the repeat timerfd.
//   • OnModifiers: updates the xkb_state modifier mask so subsequent keysym /
//     modifier lookups reflect the current state.
//   • OnLeave: disarms the repeat timerfd (Wayland protocol requires stopping
//     repeat when the surface loses keyboard focus).
//   • OnRepeatInfo: stores rate / delay.
//
// All xkb objects are reference-counted by libxkbcommon and released in the
// destructor; the handler is safe to destroy at any time.
// ══════════════════════════════════════════════════════════════════════════════

template <typename App>
class KeyboardHandler
    : public wayland::client::CWlKeyboard<KeyboardHandler<App>> {
 public:
  /// Back-pointer set by SeatManager immediately after SetupHandler() returns.
  App* app_ = nullptr;

  KeyboardHandler() noexcept
      // xkb_context_new() rarely returns nullptr (OOM), but if it does all
      // OnKeymap / OnKey paths guard on xkb_context_ / xkb_state_ being
      // non-null so the handler degrades gracefully (fd is still closed).
      : xkb_context_(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
    // A monotonic, non-blocking, close-on-exec timerfd drives key repeat.  A
    // failure here is non-fatal; repeat is simply disabled (setup_failed).
    repeat_.timer_fd =
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (repeat_.timer_fd < 0)
      repeat_.setup_failed = true;
  }

  ~KeyboardHandler() noexcept {
    if (repeat_.timer_fd >= 0) {
      const itimerspec zero{};
      timerfd_settime(repeat_.timer_fd, 0, &zero, nullptr);
      close(repeat_.timer_fd);
    }
    if (xkb_state_)
      xkb_state_unref(xkb_state_);
    if (xkb_keymap_)
      xkb_keymap_unref(xkb_keymap_);
    if (xkb_context_)
      xkb_context_unref(xkb_context_);
  }

  // Non-copyable, non-movable (owns xkb_* resources and the timerfd).
  KeyboardHandler(const KeyboardHandler&) = delete;
  KeyboardHandler& operator=(const KeyboardHandler&) = delete;
  KeyboardHandler(KeyboardHandler&&) = delete;
  KeyboardHandler& operator=(KeyboardHandler&&) = delete;

  // ── wl_keyboard events ────────────────────────────────────────────────────

  /// Receives the compositor's keymap.  Compiles it via xkbcommon and builds
  /// a fresh xkb_state.  Closes the fd in all code paths (protocol transfers
  /// ownership of the fd to the client).
  void OnKeymap(uint32_t format, int32_t fd, uint32_t size) override {
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && xkb_context_) {
      // wl_keyboard v7+ requires MAP_PRIVATE; older compositors use MAP_SHARED.
      // Default to MAP_PRIVATE when the proxy is not yet bound (e.g. a handler
      // exercised directly in a unit test) so wl_proxy_get_version is not
      // called on a null proxy.
      wl_proxy* const proxy = this->GetProxy();
      const int mflags = (proxy == nullptr || wl_proxy_get_version(proxy) >= 7)
                             ? MAP_PRIVATE
                             : MAP_SHARED;
      auto* keymap_str = static_cast<char*>(mmap(
          nullptr, static_cast<std::size_t>(size), PROT_READ, mflags, fd, 0));
      if (keymap_str != MAP_FAILED) {
        xkb_keymap* new_km = xkb_keymap_new_from_string(
            xkb_context_, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(keymap_str, static_cast<std::size_t>(size));
        if (new_km) {
          xkb_state* new_state = xkb_state_new(new_km);
          if (new_state) {
            // Only swap in the new keymap once we have a valid state for it.
            xkb_keymap_unref(xkb_keymap_);
            xkb_keymap_ = new_km;
            xkb_state_unref(xkb_state_);
            xkb_state_ = new_state;
            if (app_)
              CallOnKeymap(xkb_keymap_);
          } else {
            // State creation failed; keep old keymap+state and discard new_km.
            xkb_keymap_unref(new_km);
          }
        }
      }
    }
    if (fd >= 0)
      close(fd);
  }

  void OnEnter(uint32_t /*serial*/,
               wl_proxy* /*surface*/,
               wl_array* /*keys*/) override {}

  /// Stops key-repeat when the keyboard focus leaves the surface.
  ///
  /// The Wayland protocol requires clients to stop any ongoing key-repeat
  /// when a wl_keyboard.leave event is received. The optional
  /// App::OnKeyboardLeave() hook (detected via SFINAE) lets a consumer react to
  /// keyboard focus loss. It is deliberately named distinctly from the IME
  /// wl::ime::TextInputListener::OnLeave() so a class that is both a keyboard
  /// consumer and a text-input listener does not conflate the two.
  void OnLeave(uint32_t /*serial*/, wl_proxy* /*surface*/) override {
    StopRepeat();
    if (app_)
      CallOnKeyboardLeave(0);
  }

  /// Translates the evdev scancode to a keysym + modifier mask and delivers a
  /// KeyEvent to App::OnKey.  Arms the repeat timerfd on key-press for
  /// repeatable keys; disarms it on release of the currently repeating key.
  void OnKey(uint32_t serial,
             uint32_t /*time*/,
             uint32_t key,
             uint32_t state) override {
    if (!app_)
      return;
    // Remember the most recent key serial; a client-side repeat tick reuses it
    // (repeats carry no serial of their own).
    last_serial_ = serial;

    // wl_keyboard v10 lets the compositor drive key repeat itself, delivering
    // key events with state "repeated" (value 2) instead of advertising a rate
    // for the client to run its own timer.  Present those to the App as a
    // pressed KeyEvent flagged repeat=true, so server-driven repeat and the
    // client-side timerfd repeat below are indistinguishable to consumers.
    const bool server_repeat = state == kKeyStateRepeated;
    const uint32_t app_state =
        server_repeat ? static_cast<uint32_t>(WL_KEYBOARD_KEY_STATE_PRESSED)
                      : state;

    KeyEvent ev{key, XKB_KEY_NoSymbol, app_state, 0u, server_repeat, serial};
    xkb_keycode_t xkb_code = 0;
    if (xkb_state_) {
      // Translate Linux evdev scancode → XKB scancode (evdev offset is 8).
      xkb_code = static_cast<xkb_keycode_t>(key) + 8u;
      ev.keysym = xkb_state_key_get_one_sym(xkb_state_, xkb_code);
      ev.modifiers =
          xkb_state_serialize_mods(xkb_state_, XKB_STATE_MODS_EFFECTIVE);
    }
    app_->OnKey(ev);

    // ── Client-side repeat ─────────────────────────────────────────────────
    // Arm only for genuine presses of repeatable keys when the compositor
    // advertised a non-zero rate.  A v10 compositor that drives repeat itself
    // advertises rate 0, so this never arms and the two mechanisms never
    // double up.
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      if (xkb_keymap_ != nullptr && xkb_code != 0 &&
          xkb_keymap_key_repeats(xkb_keymap_, xkb_code) && repeat_.rate > 0 &&
          !repeat_.setup_failed) {
        repeat_.key = key;
        ArmRepeat();
      }
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
      if (repeat_.key == key)
        StopRepeat();
    }
  }

  /// Updates the xkb modifier state so subsequent keysym / modifier lookups
  /// are correct.
  void OnModifiers(uint32_t /*serial*/,
                   uint32_t mods_depressed,
                   uint32_t mods_latched,
                   uint32_t mods_locked,
                   uint32_t group) override {
    if (xkb_state_)
      xkb_state_update_mask(xkb_state_, mods_depressed, mods_latched,
                            mods_locked, 0, 0, group);
  }

  /// Stores the compositor-advertised repeat rate and delay.  A rate of zero
  /// disables repeat (per the Wayland protocol).
  void OnRepeatInfo(int32_t rate, int32_t delay) override {
    repeat_.rate = rate;
    repeat_.delay = delay;
  }

  // ── Keyboard repeat integration ───────────────────────────────────────────

  /// Returns the timerfd used for key-repeat signaling, or -1 if it could not
  /// be created.  When ≥ 0, pass it to RunEventLoop or your own poll(2) loop:
  /// call DispatchRepeat() each time it becomes readable (POLLIN).
  [[nodiscard]] int GetRepeatFd() const noexcept {
    return repeat_.setup_failed ? -1 : repeat_.timer_fd;
  }

  /// Returns false if the repeat timerfd setup failed.  When false, key-repeat
  /// is unavailable but all other keyboard events continue to function
  /// normally.
  [[nodiscard]] bool is_repeat_valid() const noexcept {
    return !repeat_.setup_failed;
  }

  /// Drain the timerfd and fire one key-repeat event on app_.
  ///
  /// Must be called from the main event-loop thread.  Delivers a KeyEvent with
  /// repeat == true; the keysym and modifier mask are re-resolved from the
  /// current xkb_state so a modifier change held mid-repeat takes effect.
  void DispatchRepeat() noexcept {
    if (!app_ || repeat_.timer_fd < 0)
      return;

    // Drain the expiration count.  O_NONBLOCK ensures this never blocks; a
    // short read / EAGAIN means there is nothing to fire this iteration.
    uint64_t expirations = 0;
    if (read(repeat_.timer_fd, &expirations, sizeof(expirations)) !=
        static_cast<ssize_t>(sizeof(expirations)))
      return;

    KeyEvent ev{
        repeat_.key, XKB_KEY_NoSymbol, WL_KEYBOARD_KEY_STATE_PRESSED, 0u,
        true,        last_serial_};
    if (xkb_state_) {
      const xkb_keycode_t xkb_code =
          static_cast<xkb_keycode_t>(repeat_.key) + 8u;
      ev.keysym = xkb_state_key_get_one_sym(xkb_state_, xkb_code);
      ev.modifiers =
          xkb_state_serialize_mods(xkb_state_, XKB_STATE_MODS_EFFECTIVE);
    }
    app_->OnKey(ev);
  }

 private:
  // wl_keyboard.key state "repeated" (value 2), added in wl_keyboard v10.  A
  // local constant so the handler also builds against pre-v10 wayland headers.
  static constexpr uint32_t kKeyStateRepeated = 2u;

  xkb_context* xkb_context_ = nullptr;
  xkb_keymap* xkb_keymap_ = nullptr;
  xkb_state* xkb_state_ = nullptr;

  // Serial of the most recent wl_keyboard.key event; reused for repeat ticks.
  uint32_t last_serial_ = 0;

  // ── Keyboard repeat state ─────────────────────────────────────────────────
  struct RepeatState {
    int32_t rate = 0;   // characters per second (0 = disabled)
    int32_t delay = 0;  // initial delay in milliseconds before first repeat
    int timer_fd = -1;  // timerfd (CLOCK_MONOTONIC), -1 when unavailable
    bool setup_failed = false;
    uint32_t key = 0;  // evdev keycode currently repeating
  } repeat_;

  // ── Repeat timer helpers ──────────────────────────────────────────────────

  /// Arm (or re-arm) the repeat timerfd for the current key.
  ///
  /// it_value is the initial delay; it_interval is the repeat period derived
  /// from the rate (characters per second → nanoseconds per character).
  void ArmRepeat() noexcept {
    if (repeat_.timer_fd < 0 || repeat_.rate <= 0)
      return;
    itimerspec in{};
    in.it_value.tv_sec = repeat_.delay / 1000;
    in.it_value.tv_nsec = static_cast<long>(repeat_.delay % 1000) * 1000000L;
    const long interval_ns = 1000000000L / repeat_.rate;
    in.it_interval.tv_sec = interval_ns / 1000000000L;
    in.it_interval.tv_nsec = interval_ns % 1000000000L;
    timerfd_settime(repeat_.timer_fd, 0, &in, nullptr);
  }

  /// Disarm the repeat timerfd without closing it.
  void StopRepeat() noexcept {
    if (repeat_.timer_fd < 0)
      return;
    const itimerspec zero{};
    timerfd_settime(repeat_.timer_fd, 0, &zero, nullptr);
  }

  // ── SFINAE optional keymap hook ───────────────────────────────────────────

  // Call app_->OnKeymap(xkb_keymap*) if the method exists.
  template <typename A = App>
  auto CallOnKeymap(xkb_keymap* km)
      -> decltype(std::declval<A&>().OnKeymap(km), void()) {
    app_->OnKeymap(km);
  }
  // Fallback: OnKeymap not present — do nothing.
  void CallOnKeymap(...) noexcept {}

  // ── SFINAE optional keyboard-leave hook ───────────────────────────────────

  // Call app_->OnKeyboardLeave() if the method exists.  The hook takes no
  // arguments, so an int/long priority tag disambiguates: a plain
  // overload-vs-variadic pair would tie on a zero-argument call and overload
  // resolution would then prefer the non-template fallback.  The present
  // overload takes an int and the fallback a long; the call passes 0 (int), so
  // the present overload wins whenever its SFINAE is satisfied.
  template <typename A = App>
  auto CallOnKeyboardLeave(int)
      -> decltype(std::declval<A&>().OnKeyboardLeave(), void()) {
    app_->OnKeyboardLeave();
  }
  // Fallback: OnKeyboardLeave not present — do nothing.
  template <typename A = App>
  void CallOnKeyboardLeave(long) noexcept {}
};

}  // namespace wl
