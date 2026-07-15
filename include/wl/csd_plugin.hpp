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
// A plugin owns the decoration chrome only — borders, title bar, buttons.
// The application paints its own content into the content rectangle; the
// plugin never touches it.
//
// ── Provided types
// ─────────────────────────────────────────────────────────────
//
// wl::csd::HitZone    — enum class identifying pointer hit-test zones
// wl::csd::Margins    — per-edge decoration thickness
// wl::csd::InputState — pointer position and window state, for themed drawing
// wl::csd::CsdPlugin  — abstract decoration plugin base class
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
//   plugin->RenderDecoration(buf, sw, sh, content_w, content_h);
//   auto zone = plugin->HitTest(mx, my, sw, sh, content_w, content_h);
#pragma once

#include <cstdint>
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
// Geometry and input state
// ══════════════════════════════════════════════════════════════════════════════

/// Decoration thickness on each edge of the content area, in pixels.
///
/// The title bar is simply `top`.  Per-edge values (rather than a single
/// border width plus a title height) let a plugin report the asymmetric
/// geometry that themed decorations need.
struct Margins {
  int left = 0;
  int right = 0;
  int top = 0;
  int bottom = 0;
};

/// Pointer position and window state, supplied by the application so the
/// plugin can render themed hover / pressed / focused / maximized states.
struct InputState {
  int pointer_x = -1;      ///< surface-local; -1 ⇒ pointer not over the surface
  int pointer_y = -1;      ///< surface-local; -1 ⇒ pointer not over the surface
  bool pressed = false;    ///< a pointer button is currently held down
  bool focused = true;     ///< the toplevel has keyboard focus
  bool maximized = false;  ///< the toplevel is maximized
};

// ══════════════════════════════════════════════════════════════════════════════
// CsdPlugin — abstract decoration plugin interface
// ══════════════════════════════════════════════════════════════════════════════

/// Abstract base class for CSD decoration plugins.
///
/// Concrete plugins override the pure virtual methods to provide
/// decoration rendering, hit-testing, and metric queries.  The interface
/// follows the plugin pattern used by libdecor: each plugin is a
/// self-contained module responsible for all aspects of the decoration
/// chrome (borders, title bar, buttons) — and for nothing else.
class CsdPlugin {
 public:
  virtual ~CsdPlugin() = default;

  CsdPlugin() = default;
  CsdPlugin(const CsdPlugin&) = delete;
  CsdPlugin& operator=(const CsdPlugin&) = delete;
  CsdPlugin(CsdPlugin&&) = default;
  CsdPlugin& operator=(CsdPlugin&&) = default;

  // ── Decoration metrics ──────────────────────────────────────────────────

  /// Decoration thickness on each edge of the content area.
  ///
  /// Not `noexcept` and not cached by the caller: a themed plugin measures
  /// this from its theme, and the answer changes when the theme does.  Query
  /// it per redraw rather than storing it.
  [[nodiscard]] virtual Margins DecorationMargins() const = 0;

  /// Total surface width required for a content area of @p content_w.
  [[nodiscard]] int SurfaceWidth(int content_w) const {
    const Margins m = DecorationMargins();
    return content_w + m.left + m.right;
  }

  /// Total surface height required for a content area of @p content_h.
  [[nodiscard]] int SurfaceHeight(int content_h) const {
    const Margins m = DecorationMargins();
    return content_h + m.top + m.bottom;
  }

  // ── State ───────────────────────────────────────────────────────────────

  /// Set the window title displayed in the title bar.
  virtual void SetTitle(std::string_view title) = 0;

  /// Update pointer position and window state for themed rendering.
  virtual void SetInputState(const InputState& state) = 0;

  // ── Rendering ─────────────────────────────────────────────────────────

  /// Render the decoration chrome (borders, title bar, buttons) into an
  /// ARGB8888 buffer with premultiplied alpha.
  ///
  /// The content rectangle — @p content_w × @p content_h at
  /// (`DecorationMargins().left`, `DecorationMargins().top`) — is left
  /// untouched for the application to paint.
  ///
  /// @param buffer     Pointer to the first pixel of the surface buffer.
  /// @param surface_w  Total surface width (content + decoration).
  /// @param surface_h  Total surface height (content + decoration).
  /// @param content_w  Content area width.
  /// @param content_h  Content area height.
  virtual void RenderDecoration(uint32_t* buffer,
                                int surface_w,
                                int surface_h,
                                int content_w,
                                int content_h) = 0;

  // ── Hit testing ───────────────────────────────────────────────────────

  /// Determine which decoration zone the pointer is over.
  [[nodiscard]] virtual HitZone HitTest(int x,
                                        int y,
                                        int surface_w,
                                        int surface_h,
                                        int content_w,
                                        int content_h) const noexcept = 0;

  // ── Event sources ─────────────────────────────────────────────────────

  /// Drain any plugin-internal event source, without blocking.
  ///
  /// A no-op for plugins that have none.  A themed plugin backed by a
  /// toolkit pumps that toolkit here so its settings / theme changes are
  /// noticed.  The application calls this once per frame.
  virtual void Dispatch() {}
};

}  // namespace wl::csd
