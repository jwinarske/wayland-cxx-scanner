// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// cursor — a wl_cursor helper that loads a cursor theme and points the seat's
// pointer at a named shape via wl_pointer.set_cursor.  It fills the gap left by
// wl::SeatManager, which owns the wl_pointer but does not manage cursors (the
// cursor surface, its wl_shm buffers, and the theme belong to the consumer, not
// to the input-plumbing layer).
//
// Typical usage with wl::SeatManager<App>:
//
//   wl::CursorManager cursor_;                        // member
//   cursor_.Init(shm_proxy, compositor_proxy);        // after BindGlobals
//
//   // The App defines a pointer hook so SeatManager binds the wl_pointer, and
//   // caches the enter serial (set_cursor must carry a wl_pointer.enter
//   serial): void OnPointerEnter(const wl::PointerEvent& ev) {
//     enter_serial_ = ev.serial;
//     cursor_.Set(seat_.Pointer(), enter_serial_, "default");
//   }
//   void OnPointerMotion(const wl::PointerEvent&) {
//     cursor_.Set(seat_.Pointer(), enter_serial_, ShapeForHit());
//   }
//
// HiDPI: call SetScale(n) (integer buffer scale) so the cursor is loaded at
// n× and presented with wl_surface.set_buffer_scale — a fractional consumer
// rounds up to the next integer for a crisp cursor.
//
// Animated cursors: a cursor with more than one image is driven by a frame
// timer (a timerfd).  Add {FrameFd(), DispatchFrame()} to the event loop
// alongside the Wayland fd (e.g. as a wl::FdSource) so the frames advance; a
// consumer that only ever uses static shapes can ignore FrameFd() (it stays
// disarmed).
//
// Links against libwayland-cursor (`wayland-cursor`).  Self-contained: it works
// through the libwayland C API, so it does not depend on the generated header.

#pragma once

extern "C" {
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-cursor.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>  // getenv, strtol
#include <cstring>  // strcmp

namespace wl {

class CursorManager {
 public:
  CursorManager() = default;
  ~CursorManager() noexcept { Release(); }

  CursorManager(const CursorManager&) = delete;
  CursorManager& operator=(const CursorManager&) = delete;
  CursorManager(CursorManager&&) = delete;
  CursorManager& operator=(CursorManager&&) = delete;

  /// Load the cursor theme (name from XCURSOR_THEME, base size from
  /// XCURSOR_SIZE or 24, times @p scale) and create the cursor surface.  @p shm
  /// and @p compositor are the bound wl_shm / wl_compositor proxies (e.g.
  /// handler.Get()->GetProxy()).  Returns false when either proxy is null, the
  /// theme cannot be loaded, or the cursor surface cannot be created; a failed
  /// Init leaves Set() a safe no-op.
  [[nodiscard]] bool Init(wl_proxy* shm,
                          wl_proxy* compositor,
                          int scale = 1) noexcept {
    Release();
    if (shm == nullptr || compositor == nullptr)
      return false;
    shm_ = shm;
    compositor_ = compositor;
    scale_ = scale > 0 ? scale : 1;

    base_size_ = 24;
    if (const char* env = std::getenv("XCURSOR_SIZE")) {
      char* end = nullptr;
      const long v = std::strtol(env, &end, 10);
      if (end != env && v > 0 && v <= 512)
        base_size_ = static_cast<int>(v);
    }
    if (!LoadTheme())
      return false;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    surface_ = wl_compositor_create_surface(
        reinterpret_cast<wl_compositor*>(compositor_));
    if (surface_ == nullptr)
      return false;
    frame_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    return true;
  }

  /// Change the integer buffer scale (HiDPI).  Reloads the theme at the new
  /// size and re-applies the current shape.  No-op if the scale is unchanged.
  void SetScale(int scale) noexcept {
    const int s = scale > 0 ? scale : 1;
    if (s == scale_ || theme_ == nullptr)
      return;
    scale_ = s;
    (void)LoadTheme();  // best effort; keeps the old theme on failure
    // Force a re-apply of the current shape at the new scale.
    const char* name = current_;
    current_ = nullptr;
    if (name != nullptr)
      Set(pointer_, serial_, name);
  }

  /// Point @p pointer (wl::SeatManager::Pointer()) at the named Xcursor shape
  /// (e.g. "default", "text", "pointer", "grab", "ns-resize", "nwse-resize"),
  /// using @p serial from the most recent wl_pointer.enter event.  Unknown
  /// names fall back to "default"/"left_ptr".  No-op if Init() did not succeed,
  /// the pointer is null, or the shape is already current (so it is cheap to
  /// call on every motion event).  Animated cursors start their frame timer.
  void Set(wl_proxy* pointer, std::uint32_t serial, const char* name) noexcept {
    if (theme_ == nullptr || surface_ == nullptr || pointer == nullptr ||
        name == nullptr)
      return;
    if (current_ != nullptr && std::strcmp(current_, name) == 0) {
      serial_ = serial;  // refresh the serial even when the shape is unchanged
      pointer_ = pointer;
      return;
    }

    wl_cursor* cursor = GetCursor(name);
    if (cursor == nullptr || cursor->image_count == 0)
      return;
    pointer_ = pointer;
    serial_ = serial;
    cursor_ = cursor;
    current_ = name;

    ApplyImage(cursor->images[0]);
    if (cursor->image_count > 1) {
      anim_start_ms_ = NowMs();
      ArmFrameTimer(cursor->images[0]->delay);
    } else {
      DisarmFrameTimer();
    }
  }

  /// The frame-timer fd for animated cursors, or -1 when none is active.  Poll
  /// it in the event loop and call DispatchFrame() when readable.
  [[nodiscard]] int FrameFd() const noexcept { return frame_fd_; }

  /// Advance an animated cursor to the frame due now and re-arm the timer.
  void DispatchFrame() noexcept {
    if (frame_fd_ >= 0) {
      std::uint64_t ticks = 0;
      const ssize_t n = ::read(frame_fd_, &ticks, sizeof(ticks));
      (void)n;
    }
    if (cursor_ == nullptr || cursor_->image_count < 2)
      return;
    const std::uint32_t elapsed =
        static_cast<std::uint32_t>(NowMs() - anim_start_ms_);
    std::uint32_t duration = 0;
    const int idx = wl_cursor_frame_and_duration(cursor_, elapsed, &duration);
    if (idx < 0 || static_cast<unsigned>(idx) >= cursor_->image_count)
      return;
    ApplyImage(cursor_->images[idx]);
    ArmFrameTimer(duration != 0 ? duration : 100);
  }

  /// Hide the cursor over the surface (set_cursor with a null surface).
  void Hide(wl_proxy* pointer, std::uint32_t serial) noexcept {
    if (pointer == nullptr)
      return;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    wl_pointer_set_cursor(reinterpret_cast<wl_pointer*>(pointer), serial,
                          nullptr, 0, 0);
    current_ = nullptr;
    cursor_ = nullptr;
    DisarmFrameTimer();
  }

  /// Forget the last-set shape so the next Set() re-applies it (call after the
  /// pointer leaves, since the compositor resets the cursor on the next enter).
  void Reset() noexcept {
    current_ = nullptr;
    cursor_ = nullptr;
    DisarmFrameTimer();
  }

  void Release() noexcept {
    if (frame_fd_ >= 0) {
      ::close(frame_fd_);
      frame_fd_ = -1;
    }
    if (surface_ != nullptr) {
      wl_surface_destroy(surface_);
      surface_ = nullptr;
    }
    if (theme_ != nullptr) {
      wl_cursor_theme_destroy(theme_);
      theme_ = nullptr;
    }
    current_ = nullptr;
    cursor_ = nullptr;
    shm_ = nullptr;
    compositor_ = nullptr;
  }

 private:
  [[nodiscard]] static std::int64_t NowMs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
  }

  [[nodiscard]] bool LoadTheme() noexcept {
    wl_cursor_theme* old = theme_;
    const char* theme_name = std::getenv("XCURSOR_THEME");  // null → default
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    theme_ = wl_cursor_theme_load(theme_name, base_size_ * scale_,
                                  reinterpret_cast<wl_shm*>(shm_));
    if (theme_ == nullptr) {
      theme_ = old;  // keep the previous theme rather than losing it
      return false;
    }
    if (old != nullptr && old != theme_)
      wl_cursor_theme_destroy(old);
    return true;
  }

  // Present one cursor image: set_cursor (hotspot in surface-local logical px),
  // attach the buffer at the current buffer scale, and commit.
  void ApplyImage(wl_cursor_image* image) noexcept {
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (buffer == nullptr)
      return;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    wl_pointer_set_cursor(reinterpret_cast<wl_pointer*>(pointer_), serial_,
                          surface_,
                          static_cast<std::int32_t>(image->hotspot_x) / scale_,
                          static_cast<std::int32_t>(image->hotspot_y) / scale_);
    wl_surface_set_buffer_scale(surface_, scale_);
    wl_surface_attach(surface_, buffer, 0, 0);
    wl_surface_damage(surface_, 0, 0, static_cast<std::int32_t>(image->width),
                      static_cast<std::int32_t>(image->height));
    wl_surface_commit(surface_);
  }

  void ArmFrameTimer(std::uint32_t delay_ms) noexcept {
    if (frame_fd_ < 0)
      return;
    const std::uint32_t d = std::max<std::uint32_t>(delay_ms, 1);
    const itimerspec spec{{0, 0},
                          {static_cast<time_t>(d / 1000),
                           static_cast<long>((d % 1000) * 1'000'000)}};
    ::timerfd_settime(frame_fd_, 0, &spec, nullptr);
  }

  void DisarmFrameTimer() noexcept {
    if (frame_fd_ < 0)
      return;
    const itimerspec spec{{0, 0}, {0, 0}};
    ::timerfd_settime(frame_fd_, 0, &spec, nullptr);
  }

  [[nodiscard]] wl_cursor* GetCursor(const char* name) const noexcept {
    wl_cursor* c = wl_cursor_theme_get_cursor(theme_, name);
    if (c == nullptr)
      c = wl_cursor_theme_get_cursor(theme_, "default");
    if (c == nullptr)
      c = wl_cursor_theme_get_cursor(theme_, "left_ptr");
    return c;
  }

  wl_cursor_theme* theme_ = nullptr;
  wl_surface* surface_ = nullptr;
  wl_proxy* shm_ = nullptr;  // retained for theme reloads on scale change
  wl_proxy* compositor_ =
      nullptr;  // retained (unused after Init, for symmetry)
  int base_size_ = 24;
  int scale_ = 1;
  int frame_fd_ = -1;  // timerfd for animated cursors

  // Current shape / animation state.
  const char* current_ = nullptr;  // last shape name (literal), for dedup
  wl_cursor* cursor_ = nullptr;    // current cursor (for frame advance)
  wl_proxy* pointer_ = nullptr;    // pointer + serial for per-frame set_cursor
  std::uint32_t serial_ = 0;
  std::int64_t anim_start_ms_ = 0;
};

}  // namespace wl
