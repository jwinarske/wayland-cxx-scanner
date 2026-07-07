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
// Links against libwayland-cursor (`wayland-cursor`).  Self-contained: it works
// through the libwayland C API, so it does not depend on the generated header.
//
// Baseline scope: uses the first image of a cursor (animated cursors show their
// first frame), a fixed theme size (XCURSOR_SIZE or 24), and no HiDPI cursor
// scaling.  Those are deliberate extensions, not part of this helper.

#pragma once

extern "C" {
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-cursor.h>
}

#include <cstdint>
#include <cstdlib>  // getenv, atoi
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

  /// Load the cursor theme (name from XCURSOR_THEME, size from XCURSOR_SIZE, or
  /// 24) and create the cursor surface.  @p shm and @p compositor are the bound
  /// wl_shm / wl_compositor proxies (e.g. handler.Get()->GetProxy()).  Returns
  /// false when either proxy is null, the theme cannot be loaded, or the cursor
  /// surface cannot be created; a failed Init leaves Set() a safe no-op.
  [[nodiscard]] bool Init(wl_proxy* shm, wl_proxy* compositor) noexcept {
    Release();
    if (shm == nullptr || compositor == nullptr)
      return false;

    int size = 24;
    if (const char* env = std::getenv("XCURSOR_SIZE")) {
      char* end = nullptr;
      const long v = std::strtol(env, &end, 10);
      if (end != env && v > 0 && v <= 512)
        size = static_cast<int>(v);
    }
    const char* theme_name = std::getenv("XCURSOR_THEME");  // null → default

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    theme_ =
        wl_cursor_theme_load(theme_name, size, reinterpret_cast<wl_shm*>(shm));
    if (theme_ == nullptr)
      return false;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    surface_ = wl_compositor_create_surface(
        reinterpret_cast<wl_compositor*>(compositor));
    return surface_ != nullptr;
  }

  /// Point @p pointer (wl::SeatManager::Pointer()) at the named Xcursor shape
  /// (e.g. "default", "text", "pointer", "grab", "ns-resize", "nwse-resize"),
  /// using @p serial from the most recent wl_pointer.enter event.  Unknown
  /// names fall back to "default"/"left_ptr".  No-op if Init() did not succeed,
  /// the pointer is null, or the shape is already current (so it is cheap to
  /// call on every motion event).
  void Set(wl_proxy* pointer, std::uint32_t serial, const char* name) noexcept {
    if (theme_ == nullptr || surface_ == nullptr || pointer == nullptr ||
        name == nullptr)
      return;
    if (current_ != nullptr && std::strcmp(current_, name) == 0)
      return;

    wl_cursor* cursor = GetCursor(name);
    if (cursor == nullptr || cursor->image_count == 0)
      return;
    wl_cursor_image* image = cursor->images[0];
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (buffer == nullptr)
      return;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    wl_pointer_set_cursor(reinterpret_cast<wl_pointer*>(pointer), serial,
                          surface_, static_cast<std::int32_t>(image->hotspot_x),
                          static_cast<std::int32_t>(image->hotspot_y));
    wl_surface_attach(surface_, buffer, 0, 0);
    wl_surface_damage(surface_, 0, 0, static_cast<std::int32_t>(image->width),
                      static_cast<std::int32_t>(image->height));
    wl_surface_commit(surface_);
    current_ = name;
  }

  /// Hide the cursor over the surface (set_cursor with a null surface).
  void Hide(wl_proxy* pointer, std::uint32_t serial) noexcept {
    if (pointer == nullptr)
      return;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    wl_pointer_set_cursor(reinterpret_cast<wl_pointer*>(pointer), serial,
                          nullptr, 0, 0);
    current_ = nullptr;
  }

  /// Forget the last-set shape so the next Set() re-applies it (call after the
  /// pointer leaves, since the compositor resets the cursor on the next enter).
  void Reset() noexcept { current_ = nullptr; }

  void Release() noexcept {
    if (surface_ != nullptr) {
      wl_surface_destroy(surface_);
      surface_ = nullptr;
    }
    if (theme_ != nullptr) {
      wl_cursor_theme_destroy(theme_);
      theme_ = nullptr;
    }
    current_ = nullptr;
  }

 private:
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
  const char* current_ = nullptr;  // last shape name (literal), for dedup
};

}  // namespace wl
