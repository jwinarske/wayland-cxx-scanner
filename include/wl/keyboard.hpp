// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// keyboard — header-only CRTP wl_keyboard handler with xkbcommon keymap
//             processing and keyboard repeat, modeled after
//             waypp/src/seat/keyboard.cc.
//
// libxkbcommon is assumed to be universally available on any system that
// runs a Wayland compositor; it is treated as a required dependency.
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated wayland_client.hpp:
//
//   #include "wayland_client.hpp"   // defines CWlKeyboard
//   #include <wl/keyboard.hpp>      // wl::KeyboardHandler<App>
//
// ── App contract ─────────────────────────────────────────────────────────────
// The App class must expose:
//
//   void OnKey(uint32_t key, uint32_t state);
//
// where `key` is the Linux evdev keycode (KEY_ESC, KEY_SPACE, …) and `state`
// is WL_KEYBOARD_KEY_STATE_PRESSED or WL_KEYBOARD_KEY_STATE_RELEASED.
//
// App may additionally provide (detected via SFINAE):
//
//   void OnKeySym(xkb_keysym_t sym, uint32_t key, uint32_t state);
//
// When OnKeySym is present it is called BEFORE OnKey on every key event.
// The `sym` is the XKB keysym translated from the current xkb_state
// (WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1); for non-XKB keymaps sym is
// XKB_KEY_NoSymbol and only OnKey is called.
//
// ── Keyboard repeat ──────────────────────────────────────────────────────────
// Keyboard repeat follows waypp's POSIX timer + signal + self-pipe pattern:
//
//   • OnRepeatInfo: on the first call, creates a POSIX real-time timer
//     (timer_create with CLOCK_REALTIME / SIGEV_SIGNAL) and installs a
//     minimal async-signal-safe signal handler on SIGRTMIN that writes one
//     byte to a self-pipe.  Subsequent calls only update the stored rate /
//     delay values; the timer and sigaction are created once per object.
//
//   • OnKey: on WL_KEYBOARD_KEY_STATE_PRESSED, arms the timer (initial delay
//     then periodic interval derived from the repeat rate) if the key is
//     repeatable (xkb_keymap_key_repeats) and the rate is non-zero.  On
//     WL_KEYBOARD_KEY_STATE_RELEASED for the currently repeating key, disarms
//     the timer.
//
//   • OnLeave: disarms the timer when the keyboard focus leaves the surface,
//     matching the Wayland protocol requirement.
//
//   • GetRepeatFd(): returns the read end of the self-pipe (-1 if setup
//     failed).  Pass this fd to RunEventLoop (or your own poll loop) so the
//     event loop wakes up when a repeat fires.
//
//   • DispatchRepeat(): drains the self-pipe and fires one repeat event
//     (OnKeySym then OnKey) on app_.  Must be called from the main
//     event-loop thread.
//
// The signal handler is async-signal-safe: it only performs a non-blocking
// write(2) and a lock-free atomic store.  All observer dispatch is deferred
// to DispatchRepeat() which runs on the normal event-loop thread.
//
// If repeat setup fails (timer_create or sigaction returns an error),
// is_repeat_valid() returns false; all other keyboard events continue to
// work normally.
// ══════════════════════════════════════════════════════════════════════════════

#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
}

#include <atomic>
#include <cstdint>

// ══════════════════════════════════════════════════════════════════════════════
// wl::KeyboardHandler<App>
//
// CRTP keyboard handler that wraps wl_keyboard and performs full xkbcommon
// keymap processing with keyboard repeat, modeled on waypp/src/seat/keyboard.cc
//
// Key design points (all matching waypp's approach):
//   • OnKeymap: mmaps the compositor-provided keymap fd (MAP_PRIVATE for
//     wl_keyboard v7+), compiles it via xkb_keymap_new_from_string, and
//     builds a fresh xkb_state.  The fd is always closed after processing,
//     as required by the Wayland protocol (fd ownership is transferred).
//   • OnKey: translates the Linux evdev scancode to an XKB scancode
//     (evdev_code + 8), looks up the keysym in the current xkb_state,
//     calls app_->OnKeySym(sym, key, state) if the method exists, then
//     always calls app_->OnKey(key, state).  Arms / disarms the repeat timer.
//   • OnModifiers: updates the xkb_state modifier mask so subsequent
//     xkb_state_key_get_one_sym calls reflect the current modifier state.
//   • OnLeave: disarms the repeat timer (Wayland protocol requires stopping
//     repeat when the surface loses keyboard focus).
//   • OnRepeatInfo: stores rate/delay; creates the POSIX timer and installs
//     the signal handler on the first call.
//
// All xkb objects are reference-counted by libxkbcommon and released in the
// destructor; the handler is safe to destroy at any time.
// ══════════════════════════════════════════════════════════════════════════════

namespace wl {

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
    // Open the self-pipe used by the repeat signal handler to wake the event
    // loop.  Both ends are non-blocking and O_CLOEXEC so the fds are not
    // leaked into child processes.  A failure here is non-fatal; repeat is
    // disabled if pipe_write_fd < 0.
    int pipefd[2] = {-1, -1};
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0) {
      repeat_.pipe_read_fd = pipefd[0];
      repeat_.pipe_write_fd = pipefd[1];
    }
  }

  ~KeyboardHandler() noexcept {
    // Cancel and destroy the repeat timer before closing the pipe so that
    // no further signal handler writes can occur after the pipe fds are gone.
    if (repeat_.timer_valid) {
      const itimerspec zero{};
      timer_settime(repeat_.timer, 0, &zero, nullptr);
      timer_delete(repeat_.timer);
    }
    if (repeat_.pipe_read_fd >= 0)
      close(repeat_.pipe_read_fd);
    if (repeat_.pipe_write_fd >= 0)
      close(repeat_.pipe_write_fd);
    if (xkb_state_)
      xkb_state_unref(xkb_state_);
    if (xkb_keymap_)
      xkb_keymap_unref(xkb_keymap_);
    if (xkb_context_)
      xkb_context_unref(xkb_context_);
  }

  // Non-copyable, non-movable (owns xkb_* resources and POSIX timer).
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
      const int mflags = (wl_proxy_get_version(this->GetProxy()) >= 7)
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
  /// when a wl_keyboard.leave event is received.
  void OnLeave(uint32_t /*serial*/, wl_proxy* /*surface*/) override {
    StopRepeat();
  }

  /// Translates the evdev scancode to an XKB keysym and dispatches to App.
  /// Calls OnKeySym(sym, key, state) if available, then OnKey(key, state).
  /// Arms the repeat timer on key-press for repeatable keys; disarms it on
  /// key-release if it is the currently repeating key.
  void OnKey(uint32_t /*serial*/,
             uint32_t /*time*/,
             uint32_t key,
             uint32_t state) override {
    if (!app_)
      return;

    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    xkb_keycode_t xkb_code = 0;

    if (xkb_state_) {
      // Translate Linux evdev scancode → XKB scancode (evdev offset is 8).
      xkb_code = static_cast<xkb_keycode_t>(key) + 8u;
      sym = xkb_state_key_get_one_sym(xkb_state_, xkb_code);
      CallOnKeySym(sym, key, state);
    }
    app_->OnKey(key, state);

    // ── Keyboard repeat ───────────────────────────────────────────────────
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      // Only arm repeat for keys the keymap marks as repeatable, and only
      // when a non-zero rate was received via OnRepeatInfo.
      if (xkb_keymap_ && xkb_code != 0 &&
          xkb_keymap_key_repeats(xkb_keymap_, xkb_code) && repeat_.rate > 0 &&
          !repeat_.setup_failed) {
        repeat_.notify.key = key;
        repeat_.notify.sym = sym;
        ArmRepeat();
      }
    } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
      if (repeat_.notify.key == key)
        StopRepeat();
    }
  }

  /// Updates the xkb modifier state so subsequent keysym lookups are correct.
  void OnModifiers(uint32_t /*serial*/,
                   uint32_t mods_depressed,
                   uint32_t mods_latched,
                   uint32_t mods_locked,
                   uint32_t group) override {
    if (xkb_state_)
      xkb_state_update_mask(xkb_state_, mods_depressed, mods_latched,
                            mods_locked, 0, 0, group);
  }

  /// Stores the compositor-advertised repeat rate and delay.  Creates the
  /// POSIX real-time timer and installs the async-signal-safe signal handler
  /// on the first call; subsequent calls only update the stored values.
  ///
  /// A rate of zero disables repeat (per the Wayland protocol).  If
  /// timer_create or sigaction fails, repeat is permanently disabled for the
  /// lifetime of this object but all other keyboard events continue normally.
  void OnRepeatInfo(int32_t rate, int32_t delay) override {
    repeat_.rate = rate;
    repeat_.delay = delay;

    // Create the timer and install the signal handler only once.
    if (repeat_.timer_valid || repeat_.setup_failed)
      return;

    sigevent sev{};
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = this;

    if (timer_create(CLOCK_REALTIME, &sev, &repeat_.timer) != 0) {
      repeat_.setup_failed = true;
      return;
    }

    // Install the signal handler.  sa_flags = SA_SIGINFO so the handler
    // receives the siginfo_t with sival_ptr identifying this object.
    //
    // NOTE: sigaction(SIGRTMIN) is process-global.  Only one KeyboardHandler
    // instance per process should call OnRepeatInfo; a second live instance
    // would overwrite this registration.  In practice a Wayland client has
    // exactly one wl_keyboard, so this is not a real limitation.
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &KeyboardHandler::RepeatSignalHandler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGRTMIN, &sa, nullptr) != 0) {
      timer_delete(repeat_.timer);
      repeat_.setup_failed = true;
      return;
    }

    repeat_.timer_valid = true;
  }

  // ── Keyboard repeat integration ───────────────────────────────────────────

  /// Returns the read end of the self-pipe used for key-repeat signalling.
  ///
  /// Returns -1 if the self-pipe could not be created (pipe2 failed) or if
  /// the repeat timer setup failed.  When ≥ 0, pass this fd to RunEventLoop
  /// or your own poll(2) loop: call DispatchRepeat() each time the fd becomes
  /// readable (POLLIN).
  [[nodiscard]] int GetRepeatFd() const noexcept {
    return repeat_.setup_failed ? -1 : repeat_.pipe_read_fd;
  }

  /// Returns false if POSIX timer or sigaction setup failed.
  ///
  /// When false, key-repeat is unavailable but all other keyboard events
  /// continue to function normally.
  [[nodiscard]] bool is_repeat_valid() const noexcept {
    return !repeat_.setup_failed;
  }

  /// Drain the self-pipe and fire one key-repeat event on app_.
  ///
  /// Must be called from the main event-loop thread (not from signal context).
  /// Calls OnKeySym(sym, key, WL_KEYBOARD_KEY_STATE_PRESSED) if available,
  /// then OnKey(key, WL_KEYBOARD_KEY_STATE_PRESSED).
  void DispatchRepeat() noexcept {
    if (!app_ || repeat_.pipe_read_fd < 0)
      return;

    // Drain all bytes written by the signal handler.  O_NONBLOCK ensures
    // this never blocks; EAGAIN / EINTR are the normal termination conditions.
    char buf[64];
    while (read(repeat_.pipe_read_fd, buf, sizeof(buf)) > 0)
      ;

    repeat_.pending.store(false, std::memory_order_relaxed);

    CallOnKeySym(repeat_.notify.sym, repeat_.notify.key,
                 WL_KEYBOARD_KEY_STATE_PRESSED);
    app_->OnKey(repeat_.notify.key, WL_KEYBOARD_KEY_STATE_PRESSED);
  }

 private:
  xkb_context* xkb_context_ = nullptr;
  xkb_keymap* xkb_keymap_ = nullptr;
  xkb_state* xkb_state_ = nullptr;

  // ── Keyboard repeat state ─────────────────────────────────────────────────
  struct RepeatState {
    int32_t rate = 0;   // characters per second (0 = disabled)
    int32_t delay = 0;  // initial delay in milliseconds before first repeat
    timer_t timer{};
    bool timer_valid = false;
    bool setup_failed = false;
    int pipe_read_fd = -1;
    int pipe_write_fd = -1;
    std::atomic<bool> pending{false};
    // Snapshot of the currently repeating key, set in OnKey and read in
    // DispatchRepeat.  Written on the main thread before arming the timer;
    // read on the main thread in DispatchRepeat.  No concurrent access.
    struct {
      uint32_t key = 0;
      xkb_keysym_t sym = XKB_KEY_NoSymbol;
    } notify;
  } repeat_;

  // ── Repeat timer helpers ──────────────────────────────────────────────────

  /// Arm (or re-arm) the repeat timer for the current key.
  ///
  /// it_value is the initial delay; it_interval is the repeat period derived
  /// from the repeat rate (characters per second → nanoseconds per character).
  /// Both timespec fields are normalised so tv_nsec is always in [0,
  /// 999999999].
  void ArmRepeat() noexcept {
    if (!repeat_.timer_valid || repeat_.rate <= 0)
      return;
    itimerspec in{};
    in.it_value.tv_sec = repeat_.delay / 1000;
    in.it_value.tv_nsec = static_cast<long>(repeat_.delay % 1000) * 1000000L;
    // interval = 1 second / rate (chars-per-second), normalised into sec+nsec.
    const long interval_ns = 1000000000L / repeat_.rate;
    in.it_interval.tv_sec = interval_ns / 1000000000L;
    in.it_interval.tv_nsec = interval_ns % 1000000000L;
    timer_settime(repeat_.timer, 0, &in, nullptr);
  }

  /// Disarm the repeat timer without destroying it.
  void StopRepeat() noexcept {
    if (!repeat_.timer_valid)
      return;
    const itimerspec zero{};
    timer_settime(repeat_.timer, 0, &zero, nullptr);
  }

  // ── Signal handler ────────────────────────────────────────────────────────

  /// Async-signal-safe repeat signal handler.
  ///
  /// POSIX restricts signal handlers to async-signal-safe functions.  This
  /// handler only sets an atomic flag (lock-free) and writes one byte to the
  /// non-blocking self-pipe.  All observer dispatch is deferred to
  /// DispatchRepeat() which runs on the main event-loop thread.
  static void RepeatSignalHandler(int /*sig*/,
                                  siginfo_t* si,
                                  void* /*uc*/) noexcept {
    if (!si || !si->si_value.sival_ptr)
      return;
    auto* self = static_cast<KeyboardHandler*>(si->si_value.sival_ptr);

    self->repeat_.pending.store(true, std::memory_order_relaxed);

    // Write one byte token.  O_NONBLOCK ensures this never blocks.  Errors
    // (EAGAIN, EINTR) are intentionally ignored — the pending flag ensures
    // DispatchRepeat will still fire even if the write is lost.
    constexpr char kToken = 1;
    const ssize_t n = write(self->repeat_.pipe_write_fd, &kToken, 1);
    static_cast<void>(n);
  }

  // ── SFINAE keysym dispatch ────────────────────────────────────────────────

  // SFINAE: call app_->OnKeySym if the method exists.
  template <typename A = App>
  auto CallOnKeySym(xkb_keysym_t sym, uint32_t key, uint32_t state)
      -> decltype(std::declval<A&>().OnKeySym(sym, key, state), void()) {
    app_->OnKeySym(sym, key, state);
  }
  // Fallback: OnKeySym not present — do nothing.
  void CallOnKeySym(...) noexcept {}
};

}  // namespace wl
