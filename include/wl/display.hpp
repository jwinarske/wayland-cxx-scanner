// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/raii.hpp>

extern "C" {
// wl_display, wl_callback, wl_callback_listener, wl_display_sync, etc.
#include <wayland-client.h>
}

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include <poll.h>

namespace wl {

/// RAII owner for a Wayland display connection.
///
/// Calls wl_display_disconnect() on destruction, replacing the common inline
/// DisplayRaii pattern used in application code:
///
/// @code
///   wl::DisplayHandle display;
///   if (!display.Connect()) { /* error */ }
///   wl::CRegistry registry;
///   registry.Create(display.Get());
/// @endcode
class DisplayHandle {
 public:
  DisplayHandle() noexcept = default;
  ~DisplayHandle() noexcept { Disconnect(); }

  DisplayHandle(const DisplayHandle&) = delete;
  DisplayHandle& operator=(const DisplayHandle&) = delete;
  DisplayHandle(DisplayHandle&&) = delete;
  DisplayHandle& operator=(DisplayHandle&&) = delete;

  /// Connect to the Wayland display named @p name.
  /// Pass nullptr (the default) to use WAYLAND_DISPLAY or "wayland-0".
  [[nodiscard]] bool Connect(const char* name = nullptr) noexcept {
    Disconnect();
    d_ = wl_display_connect(name);
    return d_ != nullptr;
  }

  /// Disconnect from the display.  Safe to call on a null handle.
  void Disconnect() noexcept {
    if (d_)
      wl_display_disconnect(std::exchange(d_, nullptr));
  }

  [[nodiscard]] wl_display* Get() const noexcept { return d_; }
  [[nodiscard]] bool IsNull() const noexcept { return d_ == nullptr; }
  explicit operator bool() const noexcept { return !IsNull(); }

 private:
  wl_display* d_ = nullptr;
};

/// Timeout-aware replacement for wl_display_roundtrip().
///
/// Creates a wl_display_sync marker, then polls the socket until the marker
/// fires or @p timeout_ms milliseconds elapse.  Never blocks longer than
/// @p timeout_ms; safe to use during start-up before the main event loop.
///
/// @param display     A connected wl_display.  Must not be null.
/// @param timeout_ms  Maximum wait in milliseconds (default: 5000 ms).
/// @returns true if the roundtrip completed; false on timeout or I/O error.
[[nodiscard]] inline bool RoundtripWithTimeout(wl_display* display,
                                               int timeout_ms = 5000) noexcept {
  bool sync_done = false;

  wl_callback* const sync_cb = wl_display_sync(display);
  if (!sync_cb)
    return false;

  const auto guard = ScopeExit{[sync_cb] { wl_callback_destroy(sync_cb); }};

  static constexpr wl_callback_listener kSyncListener = {
      [](void* data, wl_callback* /*cb*/, uint32_t /*serial*/) noexcept {
        *static_cast<bool*>(data) = true;
      }};
  wl_callback_add_listener(sync_cb, &kSyncListener, &sync_done);

  const int fd = wl_display_get_fd(display);
  bool ok = true;

  while (!sync_done && ok) {
    if (wl_display_flush(display) < 0) {
      if (errno != EAGAIN) {
        ok = false;
        break;
      }
      pollfd out{fd, POLLOUT, 0};
      if (poll(&out, 1, timeout_ms) <= 0) {
        ok = false;
        break;
      }
      continue;
    }
    pollfd in{fd, POLLIN, 0};
    const int n = poll(&in, 1, timeout_ms);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    if (wl_display_dispatch(display) < 0) {
      ok = false;
      break;
    }
  }

  return ok && sync_done;
}

/// Log a Wayland display error to stderr with application-specific context.
///
/// Distinguishes Wayland protocol errors (EPROTO) from plain I/O errors and
/// prints a descriptive message to stderr.
///
/// @param display  A connected wl_display.
/// @param context  What was happening when the error occurred (e.g., "flush").
/// @param prefix   Application name prefix for the log line (e.g., "my-app").
inline void LogWlError(wl_display* display,
                       std::string_view context,
                       std::string_view prefix = "wl") noexcept {
  const int err = wl_display_get_error(display);
  const int code = err ? err : errno;
  if (code == EPROTO) {
    const wl_interface* iface = nullptr;
    uint32_t obj_id = 0;
    const uint32_t proto_code =
        wl_display_get_protocol_error(display, &iface, &obj_id);
    std::fprintf(
        stderr,
        "%.*s: compositor protocol error (%.*s): code %u on %s object %u\n",
        static_cast<int>(prefix.size()), prefix.data(),
        static_cast<int>(context.size()), context.data(), proto_code,
        iface ? iface->name : "unknown", obj_id);
  } else {
    std::fprintf(stderr, "%.*s: compositor disconnected (%.*s): %s\n",
                 static_cast<int>(prefix.size()), prefix.data(),
                 static_cast<int>(context.size()), context.data(),
                 std::strerror(code));
  }
}

/// Run the canonical Wayland client event loop until @p should_stop() returns
/// true.
///
/// Implements the standard three-phase dispatch cycle:
///   1. **Flush** — send all queued outgoing requests; if the socket buffer is
///      full, poll for POLLOUT before retrying.
///   2. **Dispatch pending** — process events already buffered on the client
///      side without reading from the socket.
///   3. **Wait and dispatch** — block on POLLIN until the compositor sends
///      data, then dispatch the received events.
///
/// @tparam StopFn     Callable returning bool.  The loop exits when it returns
///                    true.  Must not throw.
/// @param display     A connected wl_display.  Must not be null.
/// @param should_stop Called once per iteration; return true to exit cleanly.
/// @param prefix      Application name for error log messages.
/// @returns true on a clean exit (should_stop() became true); false on I/O
///          or protocol error.
template <typename StopFn>
[[nodiscard]] inline bool RunEventLoop(
    wl_display* display,
    StopFn&& should_stop,
    std::string_view prefix = "wl") noexcept {
  const int fd = wl_display_get_fd(display);

  while (!should_stop()) {
    // ── Write phase ────────────────────────────────────────────────────────
    while (wl_display_flush(display) < 0) {
      if (errno != EAGAIN) {
        LogWlError(display, "flush", prefix);
        return false;
      }
      pollfd pfd{fd, POLLOUT, 0};
      if (poll(&pfd, 1, -1) < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
    }

    // ── Dispatch pending ───────────────────────────────────────────────────
    if (wl_display_dispatch_pending(display) < 0) {
      LogWlError(display, "dispatch_pending", prefix);
      return false;
    }

    // ── Wait for events ────────────────────────────────────────────────────
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, -1) < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }

    if (wl_display_dispatch(display) < 0) {
      LogWlError(display, "dispatch", prefix);
      return false;
    }
  }

  return true;
}

/// Run the Wayland client event loop with keyboard-repeat support.
///
/// Identical to the two-argument RunEventLoop overload, but additionally
/// polls the fd returned by @p get_repeat_fd() alongside the Wayland display
/// fd on every iteration.  Because the fd is re-evaluated each time through
/// the loop, polling of the keyboard repeat pipe is automatically enabled
/// the moment a keyboard is bound (after wl_seat::capabilities fires) and
/// disabled again if the keyboard is released — with no GLib or other
/// framework dependency.
///
/// When @p get_repeat_fd() returns -1 (no keyboard bound yet, or repeat
/// setup failed) the iteration behaves exactly like the basic overload:
/// only the Wayland fd is polled.
///
/// Whenever the repeat fd is readable (POLLIN), @p on_repeat() is called on
/// the main thread before the next Wayland dispatch.
///
/// Typical usage:
/// @code
///   wl::RunEventLoop(display_.Get(), [this] { return !running_; },
///                    "my-app",
///                    [this] { return seat_.GetRepeatFd(); },
///                    [this] { seat_.DispatchRepeat(); });
/// @endcode
///
/// @tparam StopFn      Callable returning bool.  Must not throw.
/// @tparam RepeatFdFn  Callable returning int (the repeat pipe fd, or -1).
///                     Invoked once per loop iteration.  Must not throw.
/// @tparam RepeatFn    Callable taking no arguments.  Called on the main
///                     thread each time the repeat fd is readable.  Must not
///                     throw.
/// @param display      A connected wl_display.  Must not be null.
/// @param should_stop  Called once per iteration; return true to exit cleanly.
/// @param prefix       Application name for error log messages.
/// @param get_repeat_fd  Returns the current repeat fd each iteration; e.g.
///                     [&seat]{ return seat.GetRepeatFd(); }.
/// @param on_repeat    Called when the repeat fd is readable; should call
///                     wl::SeatManager::DispatchRepeat() or
///                     wl::KeyboardHandler::DispatchRepeat().
/// @returns true on a clean exit (should_stop() became true); false on I/O
///          or protocol error.
template <typename StopFn, typename RepeatFdFn, typename RepeatFn>
[[nodiscard]] inline bool RunEventLoop(wl_display* display,
                                       StopFn&& should_stop,
                                       std::string_view prefix,
                                       RepeatFdFn&& get_repeat_fd,
                                       RepeatFn&& on_repeat) noexcept {
  const int wl_fd = wl_display_get_fd(display);

  while (!should_stop()) {
    // ── Write phase ────────────────────────────────────────────────────────
    while (wl_display_flush(display) < 0) {
      if (errno != EAGAIN) {
        LogWlError(display, "flush", prefix);
        return false;
      }
      pollfd pfd{wl_fd, POLLOUT, 0};
      if (poll(&pfd, 1, -1) < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
    }

    // ── Dispatch pending ───────────────────────────────────────────────────
    if (wl_display_dispatch_pending(display) < 0) {
      LogWlError(display, "dispatch_pending", prefix);
      return false;
    }

    // ── Wait for events (Wayland fd + repeat fd when keyboard is present) ──
    // Re-evaluate the repeat fd each iteration: it is -1 until a keyboard
    // is bound and the compositor sends wl_keyboard::repeat_info (which
    // creates the POSIX timer and the self-pipe).  Only poll it when valid.
    const int rep_fd = get_repeat_fd();
    const nfds_t nfds = (rep_fd >= 0) ? 2 : 1;
    // Always initialize both entries; poll() only examines the first nfds.
    // Keeping pfds[1].fd=-1 / events=0 when rep_fd<0 is safe with any
    // conformant poll() implementation, but explicit is clearer.
    pollfd pfds[2] = {{wl_fd, POLLIN, 0}, {-1, 0, 0}};
    if (rep_fd >= 0)
      pfds[1] = {rep_fd, POLLIN, 0};
    if (poll(pfds, nfds, -1) < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }

    // ── Keyboard repeat ────────────────────────────────────────────────────
    if (rep_fd >= 0 && (pfds[1].revents & POLLIN))
      on_repeat();

    // ── Wayland events ─────────────────────────────────────────────────────
    if (pfds[0].revents & POLLIN) {
      if (wl_display_dispatch(display) < 0) {
        LogWlError(display, "dispatch", prefix);
        return false;
      }
    }
  }

  return true;
}

}  // namespace wl
