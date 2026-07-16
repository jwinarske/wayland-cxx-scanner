// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// decorated_frame — the DecoratedWindow, packaged so an example adopts it in a
// handful of lines instead of a dozen.
//
// DecoratedWindow is deliberately unopinionated: it takes raw proxies, hands
// back a cursor name for the application to set, and leaves the wl_iface()
// definitions for the interfaces it binds to the consumer. That flexibility
// costs every consumer the same boilerplate — the cursor manager, the enter
// serial, the pointer forwarding, and six wl_iface() definitions. This wraps
// all of it:
//
//   • the six wl_iface() definitions the frame needs beyond the two every
//     windowed app already has (wl_compositor, wl_surface) — provided inline,
//     so an adopter deletes any it wrote itself and defines none of them;
//   • a small object that owns the frame, its cursor manager, and the enter
//     serial, and forwards the four pointer events with the cursor set for you.
//
// What stays in the example is the one thing that cannot be hoisted: the frame
// must Commit() before the application presents its own surface, and only the
// application knows when that is (eglSwapBuffers, a Vulkan present, an SHM
// attach+commit). So the render loop calls CommitBeforePresent() itself, right
// before it presents.
//
// Turning decoration off is still the build's business: under csd=none the
// frame is empty inlines and this wraps nothing, but the cursor still loads and
// the example compiles and runs undecorated.
#pragma once

#include "decorated_window.hpp"
#include "frame_wl_iface.hpp"

#include <wl/cursor.hpp>
#include <wl/pointer.hpp>

#include <string_view>

namespace wl::csd {

/// The window frame, its cursor, and the boilerplate that drives them.
///
/// An example holds one of these, hands it the window once, forwards its four
/// pointer events, and calls CommitBeforePresent() each frame. Everything the
/// frame needs a caller to remember — the cursor theme, the enter serial, the
/// pointer-to-frame plumbing, the close latch — lives here.
class DecoratedFrame {
 public:
  DecoratedFrame() = default;
  DecoratedFrame(const DecoratedFrame&) = delete;
  DecoratedFrame& operator=(const DecoratedFrame&) = delete;
  DecoratedFrame(DecoratedFrame&&) = delete;
  DecoratedFrame& operator=(DecoratedFrame&&) = delete;
  ~DecoratedFrame() = default;

  /// Build the frame and load the cursor theme it reports.
  ///
  /// @param shm,compositor  The application's own globals, for the cursor
  ///                        surface — the frame binds its own copies, but the
  ///                        cursor rides the application's pointer, so it is
  ///                        the application's compositor and shm it loads
  ///                        through.
  /// @returns whatever DecoratedWindow::Init did — false only leaves the window
  ///          undecorated, never half-built.
  bool Init(const DecoratedWindow::Config& config,
            std::string_view title,
            wl_proxy* shm,
            wl_proxy* compositor) {
    const bool ok = frame_.Init(config, MakeCsdPlugin());
    frame_.SetTitle(title);
    // The frame's toolkit knows the desktop's cursor theme and size; a null or
    // zero answer lets the cursor manager fall back to the environment. A
    // failed load is not fatal — the window is simply left with the
    // compositor's cursor — so its result is deliberately dropped.
    static_cast<void>(cursor_.Init(shm, compositor, 1, frame_.CursorThemeName(),
                                   frame_.CursorSize()));
    return ok;
  }

  // ── Configure plumbing — call from the matching App handlers ────────────

  /// Turn an xdg_toplevel.configure size into the content size to render at.
  void ContentSize(int width, int height, int* content_w, int* content_h) {
    frame_.ContentSizeForConfigure(width, height, content_w, content_h);
  }
  /// Feed the toplevel's configure states through, for styling and restore.
  void States(bool activated, bool maximized, bool fullscreen) noexcept {
    frame_.SetToplevelStates(activated, maximized, fullscreen);
  }
  /// The compositor's preferred scale, in 1/120 units. Rescales the decoration
  /// and, to the nearest whole buffer scale, the cursor — so an adopter gets a
  /// crisp HiDPI cursor without tracking the scale itself.
  void SetScale(int scale_120) {
    frame_.SetScale(scale_120);
    const int unity = 120;  // ScalePolicy::kUnityScale120, kept local here
    cursor_.SetScale(scale_120 > 0 ? (scale_120 + unity / 2) / unity : 1);
  }

  /// Draw and commit the decoration for a content area of @p content_w by
  /// @p content_h. Call once per frame, immediately before the application
  /// presents its own surface: the frame's subsurface is synchronized, so it
  /// reaches the screen with that present and stays in step through a resize.
  void CommitBeforePresent(int content_w, int content_h) {
    frame_.Dispatch();
    frame_.Commit(content_w, content_h);
  }

  // ── Pointer — call from the App's four pointer hooks ────────────────────
  //
  // The frame drives move, resize, maximize and minimize itself; close is the
  // only gesture handed back, because only the application can decide to exit.
  // The cursor is set here because the pointer is the application's, and the
  // frame answers a shape only over its own surface.

  void PointerEnter(const wl::PointerEvent& ev, wl_proxy* pointer) noexcept {
    enter_serial_ = ev.serial;
    frame_.OnPointerEnter({ev.x, ev.y, ev.serial, ev.time, ev.surface});
    cursor_.Reset();  // the compositor drops the cursor on enter; re-apply
    UpdateCursor(pointer);
  }
  void PointerLeave() noexcept { frame_.OnPointerLeave(); }
  void PointerMotion(const wl::PointerEvent& ev, wl_proxy* pointer) noexcept {
    frame_.OnPointerMotion({ev.x, ev.y, ev.serial, ev.time, nullptr});
    UpdateCursor(pointer);
  }
  /// @returns true once the user has activated the close button; the caller
  ///          stops its loop.
  [[nodiscard]] bool PointerButton(const wl::PointerButtonEvent& ev) noexcept {
    frame_.OnPointerButton({ev.serial, ev.time, ev.button, ev.state});
    return frame_.CloseRequested();
  }

  // ── Animated cursors (optional) ─────────────────────────────────────────
  //
  // A themed cursor may animate; add these to the event loop to advance it. An
  // example that ignores them simply shows the first frame, which is fine.
  [[nodiscard]] int CursorFrameFd() const noexcept { return cursor_.FrameFd(); }
  void DispatchCursorFrame() noexcept { cursor_.DispatchFrame(); }

  /// The frame itself, for the rare caller that needs it directly (margins,
  /// DrawsClientSide()); most never do.
  [[nodiscard]] DecoratedWindow& Window() noexcept { return frame_; }

 private:
  void UpdateCursor(wl_proxy* pointer) noexcept {
    const char* name = frame_.CursorName();
    cursor_.Set(pointer, enter_serial_, name != nullptr ? name : "default");
  }

  DecoratedWindow frame_;
  wl::CursorManager cursor_;
  uint32_t enter_serial_ = 0;
};

}  // namespace wl::csd
