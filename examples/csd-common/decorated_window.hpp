// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// decorated_window — a client-side window frame on its own subsurface.
//
// The CsdPlugin interface paints decoration into a buffer the application
// already owns, which suits a client that renders its own pixels and suits
// nothing else: an EGL or Vulkan client renders on the GPU and has no buffer to
// paint into.  This puts the decoration on a wl_subsurface of its own, backed
// by wl_shm, sitting behind the content surface — so the application goes on
// rendering however it likes and never sees the frame at all.
//
// ── What it owns / what the application owns ─────────────────────────────────
//
//   this          the decoration surface, its subsurface, its SHM buffers, and
//                 the plugin that draws them
//   application   its own content surface, and the xdg_surface/xdg_toplevel on
//                 it — the decoration is a child of that surface, so the
//                 toplevel stays the application's
//
// ── Geometry ─────────────────────────────────────────────────────────────────
//
// The content surface is the origin.  The decoration surface is placed at
// (-margins.left, -margins.top) and extends to cover the content, which sits on
// top of it; the plugin leaves the content rectangle untouched, so nothing is
// drawn twice.  A window's visible bounds — what goes in
// xdg_surface.set_window_geometry — are therefore
//
//     (0, -VisibleMargins().top, content_w, VisibleMargins().top + content_h)
//
// in the content surface's coordinates, which WindowGeometry() returns.
//
// ── What the consumer must provide ───────────────────────────────────────────
//
// This is a library, and wl_iface() is defined per-consumer by convention here
// — so it cannot define them itself without colliding with the very example
// linking it. A consumer must therefore define wl_iface() for the interfaces
// the frame uses, on top of its own:
//
//   wl_compositor, wl_surface, wl_shm, wl_shm_pool, wl_buffer,
//   wl_subcompositor, wl_subsurface
//
// Omitting one is a link error naming exactly what is missing.
//
// ── Input ────────────────────────────────────────────────────────────────────
//
// A subsurface receives its own pointer events, so the application must route
// them: OwnsSurface() identifies them, and the coordinates that arrive with
// them are already decoration-local, which is what HitTest() wants.
#pragma once

#include "csd_factory.hpp"

#include <wl/csd_plugin.hpp>

extern "C" {
#include <wayland-client-core.h>
}

#include <cstdint>
#include <memory>
#include <string_view>

namespace wl::csd {

// The pointer events the frame needs, decoupled from <wl/pointer.hpp> so this
// header does not drag the seat machinery into every caller. A caller fills
// these from whatever its own pointer handling delivers.
struct PointerEventLite {
  double x = 0.0;
  double y = 0.0;
  uint32_t serial = 0;
  uint32_t time = 0;
  const wl_proxy* surface = nullptr;  ///< enter/leave only; null for motion
};

struct PointerButtonEventLite {
  uint32_t serial = 0;
  uint32_t time = 0;
  uint32_t button = 0;  ///< BTN_LEFT etc.
  uint32_t state = 0;   ///< WL_POINTER_BUTTON_STATE_*
};

/// A decoration frame on a subsurface of the application's content surface.
class DecoratedWindow {
 public:
  DecoratedWindow();
  ~DecoratedWindow();

  DecoratedWindow(const DecoratedWindow&) = delete;
  DecoratedWindow& operator=(const DecoratedWindow&) = delete;
  DecoratedWindow(DecoratedWindow&&) = delete;
  DecoratedWindow& operator=(DecoratedWindow&&) = delete;

  /// What the frame needs to do its job. Anything optional may be null.
  struct Config {
    wl_proxy* compositor = nullptr;
    wl_proxy* subcompositor = nullptr;
    wl_proxy* shm = nullptr;
    /// zxdg_decoration_manager_v1. Null when the compositor offers none, which
    /// means it will not decorate and the plugin is the only chance of a frame.
    wl_proxy* decoration_manager = nullptr;
    /// The seat backing interactive move and resize; null disables both.
    wl_proxy* seat = nullptr;
    wl_proxy* content_surface = nullptr;
    /// Needed for move, resize, maximize and minimize, which this drives
    /// itself; null leaves the frame drawn but inert.
    wl_proxy* xdg_toplevel = nullptr;
    /// Whether to let the compositor decorate if it will, using the plugin only
    /// if it declines. Defaults to what the csd option asked for, which is what
    /// a caller wants unless it has a reason of its own.
    bool prefer_server_side = CsdPrefersServerSide();
  };

  /// Create the decoration and negotiate who draws it.
  ///
  /// The negotiation is the frame's, not the caller's: a caller that had to
  /// decide would need to know which plugin the build chose and what the
  /// compositor answered, which is exactly what this exists to hide. With no
  /// plugin it asks the compositor to decorate; with one it asks to do the job
  /// itself, unless the build said to prefer the compositor.
  ///
  /// @param plugin  The decoration to draw, or null for none. Null is a
  ///                supported state, not an error.
  /// @returns false only if something needed was missing, leaving the window
  ///          undecorated rather than half-built.
  [[nodiscard]] bool Init(const Config& config,
                          std::unique_ptr<CsdPlugin> plugin);

  /// True when this client is drawing the frame — i.e. there is a plugin and
  /// the compositor did not take the job. False means the window has either a
  /// compositor-drawn frame or none, and either way nothing here draws.
  [[nodiscard]] bool DrawsClientSide() const noexcept;

  // ── Geometry ───────────────────────────────────────────────────────────

  /// Total decoration thickness around the content, in logical pixels. Zero
  /// on every edge when there is no plugin.
  [[nodiscard]] Margins DecorationMargins() const;

  /// The decoration that is part of the window rather than shadow.
  [[nodiscard]] Margins VisibleMargins() const;

  /// The window's visible bounds in the content surface's coordinates, for
  /// xdg_surface.set_window_geometry. `y` is negative: the title bar is above
  /// the content surface's origin.
  struct Geometry {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };
  [[nodiscard]] Geometry WindowGeometry(int content_w, int content_h) const;

  /// Content size for a configure carrying @p width by @p height — the size of
  /// the window geometry, not of any surface.
  void ContentSizeForConfigure(int width,
                               int height,
                               int* content_w,
                               int* content_h) const;

  // ── State ──────────────────────────────────────────────────────────────

  void SetTitle(std::string_view title);
  void SetScale(int scale_120);

  /// Feed the toplevel's configure states through, so the frame can render
  /// active/backdrop and the maximize button's icon the way the theme wants.
  /// The compositor is the only authority on either.
  void SetToplevelStates(bool activated, bool maximized) noexcept;

  /// Drain the plugin's own event source. Call once per frame.
  void Dispatch();

  // ── Frame ──────────────────────────────────────────────────────────────

  /// Redraw and commit the decoration for a content area of @p content_w by
  /// @p content_h.
  ///
  /// The subsurface stays synchronized, so this does not appear on screen
  /// until the application commits its content surface — which is what keeps
  /// the frame and the content in step through a resize.
  void Commit(int content_w, int content_h);

  // ── Input ──────────────────────────────────────────────────────────────
  //
  // Forward every pointer event; the frame takes the ones on its own surface
  // and ignores the rest, so a caller needs no test of its own. It drives
  // move, resize, maximize and minimize itself — it has the toplevel — and
  // only close comes back, because only the application can decide to exit.
  //
  // The gestures live here rather than in each caller because they are not
  // simple: a button fires on release over the button it was pressed on, a
  // title-bar press becomes a move only once the pointer travels past the
  // toolkit's drag threshold, and a second press within the toolkit's
  // double-click time maximizes instead. Every caller reimplementing that
  // would get a different subset of it right.

  void OnPointerEnter(const PointerEventLite& ev) noexcept;
  void OnPointerLeave() noexcept;
  void OnPointerMotion(const PointerEventLite& ev) noexcept;
  void OnPointerButton(const PointerButtonEventLite& ev) noexcept;

  /// True once the user has activated the close button. Latched: the caller
  /// polls it and stops.
  [[nodiscard]] bool CloseRequested() const noexcept;

  /// The cursor shape for wherever the pointer is, or null when it is not over
  /// the frame and the caller's own cursor applies.
  [[nodiscard]] const char* CursorName() const noexcept;

  /// The serial of the last wl_pointer.enter on the frame, which a caller
  /// needs to set a cursor.
  [[nodiscard]] uint32_t EnterSerial() const noexcept;

  /// True if @p surface is the decoration's.
  [[nodiscard]] bool OwnsSurface(const wl_proxy* surface) const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wl::csd
