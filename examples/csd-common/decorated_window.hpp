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
// drawn twice.
//
// A window's visible bounds — what goes in xdg_surface.set_window_geometry —
// are therefore the content inset by the *visible* margins, the shadow being
// outside the window rather than part of it.  Both halves of that arithmetic
// live here: Commit() declares the geometry and ContentSizeForConfigure()
// reads the configure that comes back sized to it.  They have to be exact
// inverses, so nothing is gained by letting a caller do either one.
//
// ── What the consumer must provide ───────────────────────────────────────────
//
// A display, its content surface, and the xdg_surface/xdg_toplevel on it. The
// frame binds everything else from a registry of its own.
//
// One piece of boilerplate remains, and it is the repo's rather than this
// header's: wl_iface() is defined per-consumer by convention, so a library
// cannot define them without colliding with the very example linking it. A
// consumer must therefore define wl_iface() for the interfaces the frame binds
// and uses, on top of its own:
//
//   wl_compositor, wl_surface, wl_shm, wl_shm_pool, wl_buffer,
//   wl_subcompositor, wl_subsurface, wl_region
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

#include <algorithm>
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

#ifdef CSD_ENABLED

/// A decoration frame on a subsurface of the application's content surface.
class DecoratedWindow {
 public:
  DecoratedWindow();
  ~DecoratedWindow();

  DecoratedWindow(const DecoratedWindow&) = delete;
  DecoratedWindow& operator=(const DecoratedWindow&) = delete;
  DecoratedWindow(DecoratedWindow&&) = delete;
  DecoratedWindow& operator=(DecoratedWindow&&) = delete;

  /// What the frame needs to do its job.
  ///
  /// It binds wl_compositor, wl_subcompositor, wl_shm and the decoration
  /// manager from a registry of its own rather than taking them here: a caller
  /// that had to pass them would have to bind globals it never uses, and one
  /// that forgot the decoration manager would silently lose the negotiation.
  /// A second wl_registry is ordinary — the protocol allows any number.
  struct Config {
    /// The connection. Init binds what it needs from it and round-trips once.
    wl_display* display = nullptr;
    wl_proxy* content_surface = nullptr;
    /// Where the window geometry goes; null leaves it unset, which makes the
    /// decoration part of the window's visible bounds and mis-sizes it.
    wl_proxy* xdg_surface = nullptr;
    /// Needed for move, resize, maximize and minimize, which this drives
    /// itself; null leaves the frame drawn but inert.
    wl_proxy* xdg_toplevel = nullptr;
    /// The seat backing interactive move and resize; null disables both.
    wl_proxy* seat = nullptr;
    /// The content size the window starts at — the caller's own default, in
    /// logical pixels, decoration excluded.
    ///
    /// Required. It seeds the size the frame restores to, and the first
    /// configure a compositor sends is often one with a zero axis, meaning "you
    /// pick" — so a frame that was never told will pick nothing and the window
    /// comes up a single pixel.
    int content_width = 0;
    int content_height = 0;
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

  /// True when this client is drawing the frame — i.e. there is a plugin, the
  /// compositor did not take the job, and the window is not fullscreen. False
  /// means the window has a compositor-drawn frame, or none, or wants none;
  /// either way nothing here draws and every margin below is zero.
  ///
  /// A fullscreen window has no frame on any compositor: the whole output is
  /// the content, and a title bar over it would be both wrong and in the way.
  [[nodiscard]] bool DrawsClientSide() const noexcept;

  // ── Geometry ───────────────────────────────────────────────────────────

  /// Total decoration thickness around the content, in logical pixels. Zero
  /// on every edge when there is no plugin.
  [[nodiscard]] Margins DecorationMargins() const;

  /// The decoration that is part of the window rather than shadow.
  [[nodiscard]] Margins VisibleMargins() const;

  /// Turn an xdg_toplevel.configure into a content size.
  ///
  /// The configure carries the size of the *window geometry* — which includes
  /// the decoration — not the size of any surface, and Commit() is what sets
  /// that geometry. The two are exact inverses of each other, so they live
  /// together here: a caller doing half the arithmetic and this doing the other
  /// half is how a window ends up resizing itself by the decoration on every
  /// round trip.
  ///
  /// A zero @p width or @p height means the compositor has no opinion and the
  /// size is the client's to pick, which is how an un-maximize arrives. This
  /// answers the size the window last had before the compositor imposed one;
  /// it tracks that itself, from the states given to SetToplevelStates.
  ///
  /// @param content_w,content_h  Never null; always written.
  void ContentSizeForConfigure(int width,
                               int height,
                               int* content_w,
                               int* content_h);

  // ── State ──────────────────────────────────────────────────────────────

  void SetTitle(std::string_view title);
  void SetScale(int scale_120);

  /// Feed the toplevel's configure states through. The frame renders
  /// active/backdrop and the maximize button's icon from them, and needs
  /// @p maximized and @p fullscreen to know whether a configure's size is the
  /// compositor's or one it should remember to come back to.
  ///
  /// The compositor is the only authority on any of these: tracking them from
  /// our own button clicks would be optimistic, since a maximize can be refused
  /// and can equally arrive from a keybinding we never saw.
  void SetToplevelStates(bool activated,
                         bool maximized,
                         bool fullscreen) noexcept;

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

#else  // !CSD_ENABLED

/// The same window with the decoration compiled out (csd=none).
///
/// Every decoration call below is an empty inline: nothing is linked, no
/// subsurface is created, no decoration protocol is bound, no toolkit is
/// needed, and the compiler leaves nothing behind. The point is that a caller
/// is written once against the interface above and needs no #ifdef of its own —
/// turning decoration off is the build's business, not the application's.
///
/// One thing here is deliberately *not* a no-op. A window still has to answer a
/// configure, and still has to remember the size to go back to when the
/// compositor stops imposing one — that is xdg-shell's business rather than the
/// decoration's, and it is the same arithmetic either way, only with every
/// margin zero. No-oping it would quietly take un-maximize away from exactly
/// the builds that asked for no decoration.
class DecoratedWindow {
 public:
  DecoratedWindow() = default;
  ~DecoratedWindow() = default;

  DecoratedWindow(const DecoratedWindow&) = delete;
  DecoratedWindow& operator=(const DecoratedWindow&) = delete;
  DecoratedWindow(DecoratedWindow&&) = delete;
  DecoratedWindow& operator=(DecoratedWindow&&) = delete;

  /// Mirrors the decorated Config field for field, so a caller compiles either
  /// way. Everything but the content size is ignored.
  struct Config {
    wl_display* display = nullptr;
    wl_proxy* content_surface = nullptr;
    wl_proxy* xdg_surface = nullptr;
    wl_proxy* xdg_toplevel = nullptr;
    wl_proxy* seat = nullptr;
    int content_width = 0;
    int content_height = 0;
    bool prefer_server_side = false;
  };

  /// Never fails: there is nothing to build. The window is decorated by the
  /// compositor or not at all, which is what csd=none asks for.
  [[nodiscard]] bool Init(const Config& config,
                          std::unique_ptr<CsdPlugin> /*plugin*/) {
    restore_w_ = std::max(1, config.content_width);
    restore_h_ = std::max(1, config.content_height);
    return true;
  }

  [[nodiscard]] bool DrawsClientSide() const noexcept { return false; }
  [[nodiscard]] Margins DecorationMargins() const { return {}; }
  [[nodiscard]] Margins VisibleMargins() const { return {}; }

  /// The decorated version's arithmetic with every margin zero.
  void ContentSizeForConfigure(int width,
                               int height,
                               int* content_w,
                               int* content_h) {
    static constexpr int kMaxDim = 16384;
    int cw = width > 0 ? std::min(width, kMaxDim) : restore_w_;
    int ch = height > 0 ? std::min(height, kMaxDim) : restore_h_;
    cw = std::clamp(cw, 1, kMaxDim);
    ch = std::clamp(ch, 1, kMaxDim);
    if (!maximized_ && !fullscreen_) {
      restore_w_ = cw;
      restore_h_ = ch;
    }
    *content_w = cw;
    *content_h = ch;
  }

  void SetTitle(std::string_view /*title*/) {}
  void SetScale(int /*scale_120*/) {}

  void SetToplevelStates(bool /*activated*/,
                         bool maximized,
                         bool fullscreen) noexcept {
    // Tracked even here: they decide whether a configure's size is one to
    // remember, which is the arithmetic above.
    maximized_ = maximized;
    fullscreen_ = fullscreen;
  }

  void Dispatch() {}
  void Commit(int /*content_w*/, int /*content_h*/) {}

  void OnPointerEnter(const PointerEventLite& /*ev*/) noexcept {}
  void OnPointerLeave() noexcept {}
  void OnPointerMotion(const PointerEventLite& /*ev*/) noexcept {}
  void OnPointerButton(const PointerButtonEventLite& /*ev*/) noexcept {}

  [[nodiscard]] bool CloseRequested() const noexcept { return false; }
  [[nodiscard]] const char* CursorName() const noexcept { return nullptr; }
  [[nodiscard]] uint32_t EnterSerial() const noexcept { return 0; }
  [[nodiscard]] bool OwnsSurface(const wl_proxy* /*surface*/) const noexcept {
    return false;
  }

 private:
  int restore_w_ = 1;
  int restore_h_ = 1;
  bool maximized_ = false;
  bool fullscreen_ = false;
};

#endif  // CSD_ENABLED

}  // namespace wl::csd
