// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_plugin — Abstract interface for client-side decoration (CSD) plugins.
//
// Following the plugin pattern from libdecor
// (https://gitlab.freedesktop.org/libdecor/libdecor/), this header defines
// the abstract base class that every CSD plugin must implement.  Concrete
// plugins (e.g. the header-only fallback in <wl/csd_fallback.hpp> or the
// GTK-themed plugin in <wl/csd_gtk.hpp>) override these virtual methods to
// provide decoration rendering, hit-testing, and metric queries.
//
// ── Provided types
// ─────────────────────────────────────────────────────────────
//
// wl::csd::HitZone   — enum class identifying pointer hit-test zones
// wl::csd::CsdPlugin — abstract decoration plugin base class
//
// ── Usage
// ────────────────────────────────────────────────────────────────────────
//
//   #include <wl/csd_plugin.hpp>
//   #include <wl/csd_fallback.hpp>          // or <wl/csd_gtk.hpp>
//
//   auto plugin = std::make_unique<wl::csd::FallbackCsdPlugin>();
//   plugin->SetTitle("My Window");
//   int sw = plugin->SurfaceWidth(content_w);
//   plugin->RenderFrame(buf, sw, sh, content_w, content_h, time);
//   auto zone = plugin->HitTest(mx, my, sw, sh, content_w, content_h);
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// Hit-test zones
// ══════════════════════════════════════════════════════════════════════════════

/// Identifies the CSD region under the pointer.
enum class HitZone {
  None,
  TitleBar,
  CloseButton,
  MaximizeButton,
  MinimizeButton,
  ResizeTop,
  ResizeBottom,
  ResizeLeft,
  ResizeRight,
  ResizeTopLeft,
  ResizeTopRight,
  ResizeBottomLeft,
  ResizeBottomRight,
  Content,
};

/// Map a HitZone to the xdg_toplevel resize_edge value (0 ⇒ not a resize).
inline uint32_t HitZoneToResizeEdge(HitZone zone) noexcept {
  switch (zone) {
    case HitZone::ResizeTop:
      return 1;
    case HitZone::ResizeBottom:
      return 2;
    case HitZone::ResizeLeft:
      return 4;
    case HitZone::ResizeRight:
      return 8;
    case HitZone::ResizeTopLeft:
      return 5;
    case HitZone::ResizeTopRight:
      return 9;
    case HitZone::ResizeBottomLeft:
      return 6;
    case HitZone::ResizeBottomRight:
      return 10;
    default:
      return 0;
  }
}

/// Map a HitZone to an Xcursor shape name for wl::CursorManager::Set (resize
/// edges get directional resize cursors, the window buttons a pointer/hand, and
/// everything else the default arrow).
inline const char* HitZoneToCursorName(HitZone zone) noexcept {
  switch (zone) {
    case HitZone::ResizeTop:
    case HitZone::ResizeBottom:
      return "ns-resize";
    case HitZone::ResizeLeft:
    case HitZone::ResizeRight:
      return "ew-resize";
    case HitZone::ResizeTopLeft:
    case HitZone::ResizeBottomRight:
      return "nwse-resize";
    case HitZone::ResizeTopRight:
    case HitZone::ResizeBottomLeft:
      return "nesw-resize";
    case HitZone::CloseButton:
    case HitZone::MaximizeButton:
    case HitZone::MinimizeButton:
      return "pointer";
    default:
      return "default";
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// CsdPlugin — abstract decoration plugin interface
// ══════════════════════════════════════════════════════════════════════════════

/// Abstract base class for CSD decoration plugins.
///
/// Concrete plugins override the pure virtual methods to provide
/// decoration rendering, hit-testing, and metric queries.  The interface
/// follows the plugin pattern used by libdecor: each plugin is a
/// self-contained module responsible for all aspects of the decoration
/// chrome (borders, title bar, buttons).
class CsdPlugin {
 public:
  virtual ~CsdPlugin() = default;

  CsdPlugin() = default;
  CsdPlugin(const CsdPlugin&) = delete;
  CsdPlugin& operator=(const CsdPlugin&) = delete;
  CsdPlugin(CsdPlugin&&) = default;
  CsdPlugin& operator=(CsdPlugin&&) = default;

  // ── Decoration metrics ──────────────────────────────────────────────────

  /// Width of the left/right/bottom border in pixels.
  [[nodiscard]] virtual int BorderWidth() const noexcept = 0;

  /// Height of the title bar in pixels.
  [[nodiscard]] virtual int TitleBarHeight() const noexcept = 0;

  /// Total surface width required for a content area of @p content_w.
  [[nodiscard]] int SurfaceWidth(int content_w) const noexcept {
    return content_w + 2 * BorderWidth();
  }

  /// Total surface height required for a content area of @p content_h.
  [[nodiscard]] int SurfaceHeight(int content_h) const noexcept {
    return content_h + TitleBarHeight() + BorderWidth();
  }

  // ── State ───────────────────────────────────────────────────────────────

  /// Set the window title displayed in the title bar.
  virtual void SetTitle(std::string_view title) = 0;

  /// Update window state flags (focused, maximized) for themed rendering.
  virtual void SetState(bool focused, bool maximized) = 0;

  // ── Rendering ─────────────────────────────────────────────────────────

  /// Render the full decoration frame (borders + title bar + buttons) and
  /// content area into an XRGB8888 buffer.
  ///
  /// @param buffer     Pointer to the first pixel of the surface buffer.
  /// @param surface_w  Total surface width (content + borders).
  /// @param surface_h  Total surface height (content + title bar + border).
  /// @param content_w  Content area width.
  /// @param content_h  Content area height.
  /// @param time       Animation time in milliseconds.
  virtual void RenderFrame(uint32_t* buffer,
                           int surface_w,
                           int surface_h,
                           int content_w,
                           int content_h,
                           uint32_t time) = 0;

  // ── Hit testing ───────────────────────────────────────────────────────

  /// Determine which decoration zone the pointer is over.
  [[nodiscard]] virtual HitZone HitTest(int x,
                                        int y,
                                        int surface_w,
                                        int surface_h,
                                        int content_w,
                                        int content_h) const noexcept = 0;
};

}  // namespace wl::csd
