// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_plugin — Abstract interface for client-side decoration (CSD) plugins.
//
// This header defines the abstract base class that every CSD plugin must
// implement.  Concrete plugins override these virtual methods to provide
// decoration rendering, hit-testing, and metric queries: the header-only
// fallback in <wl/csd_fallback.hpp>, and the themed plugins, which are not
// framework headers because they need a source file and a toolkit to link.
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
/// decoration rendering, hit-testing, and metric queries.  Each plugin is a
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

  /// Total decoration thickness on each edge of the content area — every
  /// pixel of surface the plugin needs, visible or not.
  ///
  /// Not `noexcept` and not cached by the caller: a themed plugin measures
  /// this from its theme, and the answer changes when the theme does.  Query
  /// it per redraw rather than storing it.
  [[nodiscard]] virtual Margins DecorationMargins() const = 0;

  /// The part of DecorationMargins() that is not the window.
  ///
  /// A drop shadow is drawn outside the window's visible bounds, and is
  /// grabbable but not part of the window: it must be excluded from
  /// xdg_surface.set_window_geometry, or the compositor will align and
  /// constrain the window as though the shadow were part of it.
  ///
  /// Must not exceed DecorationMargins() on any edge.  Zero means every pixel
  /// of the decoration is visible window.
  [[nodiscard]] virtual Margins ShadowMargins() const { return {}; }

  /// The decoration that is part of the window: DecorationMargins() minus
  /// ShadowMargins().  This is what a configure's size has to be read against,
  /// because the compositor sizes the window geometry, not the surface.
  [[nodiscard]] Margins VisibleMargins() const {
    const Margins m = DecorationMargins();
    const Margins s = ShadowMargins();
    return {m.left - s.left, m.right - s.right, m.top - s.top,
            m.bottom - s.bottom};
  }

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

  /// Set the output scale the decoration is rendered for, in 1/120 units —
  /// the form wp_fractional_scale_v1 delivers, where 120 is unity and an
  /// integer wl_output.scale N is N * 120.  See <wl/scale_policy.hpp>.
  ///
  /// Everything else in this interface is in logical pixels, this included:
  /// the scale says how many physical pixels a logical one is worth, so the
  /// decoration can be drawn at the panel's real resolution rather than
  /// drawn small and stretched.  Margins do not change with it.
  virtual void SetScale(int scale_120) { static_cast<void>(scale_120); }

  // ── Input gesture parameters ──────────────────────────────────────────

  /// Interval within which two presses count as a double-click, in ms.
  ///
  /// A desktop-wide user setting, so a themed plugin answers with the
  /// toolkit's value rather than this fallback.
  [[nodiscard]] virtual int DoubleClickTimeMs() const { return 400; }

  /// Distance the pointer must travel before a press becomes a drag, in px.
  ///
  /// Also a toolkit setting: below it a press on the title bar is still a
  /// click and may yet become a double-click, above it the press is a move.
  [[nodiscard]] virtual int DragThreshold() const { return 8; }

  // ── Rendering ─────────────────────────────────────────────────────────

  /// Render the decoration chrome (shadow, title bar, buttons) into an
  /// ARGB8888 buffer with premultiplied alpha.
  ///
  /// Every dimension here is logical.  The buffer behind them is physical —
  /// scaled by SetScale() — and @p stride_px is what connects the two, since
  /// it is the one measurement that cannot be derived from the logical size.
  ///
  /// The content rectangle — @p content_w × @p content_h at
  /// (`DecorationMargins().left`, `DecorationMargins().top`) — is left
  /// untouched for the application to paint.
  ///
  /// @param buffer     Pointer to the first pixel of the surface buffer.
  /// @param stride_px  Buffer stride, in pixels, not bytes.
  /// @param surface_w  Total logical surface width (content + decoration).
  /// @param surface_h  Total logical surface height (content + decoration).
  /// @param content_w  Logical content area width.
  /// @param content_h  Logical content area height.
  virtual void RenderDecoration(uint32_t* buffer,
                                int stride_px,
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
