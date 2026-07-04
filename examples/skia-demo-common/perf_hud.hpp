// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// PerfHud — a small on-screen overlay that draws live frame statistics (FPS,
// present latency, measured refresh) sourced from FramePacer, plus FpsMeter,
// the allocation-free rolling-window FPS counter that feeds it.
//
// The two are split so FpsMeter is pure logic (no Skia) and unit-testable on
// its own; PerfHud owns only the Skia text rendering.

#pragma once

#include <array>
#include <cstddef>

#include "include/core/SkFont.h"
#include "include/core/SkRect.h"

class SkCanvas;

namespace demo {

class FramePacer;

// Rolling frames-per-second over a fixed time window, computed from per-frame
// timestamps.  Fixed-capacity ring, so it never allocates; if frames arrive
// faster than kCapacity within the window the reported rate saturates there.
class FpsMeter {
 public:
  static constexpr double kWindowMs = 1000.0;
  static constexpr std::size_t kCapacity = 240;  // caps at 240 fps/1 s window

  // Record a frame presented/rendered at now_ms (any monotonic ms clock).
  void Tick(double now_ms) noexcept;

  // Frames within the last kWindowMs — i.e. the current FPS.  With a 1 s window
  // this is simply the retained sample count.
  [[nodiscard]] double fps() const noexcept {
    return static_cast<double>(count_);
  }

 private:
  std::array<double, kCapacity> stamps_{};
  std::size_t head_ = 0;   // index of the oldest retained stamp
  std::size_t count_ = 0;  // number of retained stamps
};

// On-screen performance overlay.  Hidden by default; the example toggles it.
// Rendering is a no-op while hidden, so it costs nothing when off.
class PerfHud {
 public:
  void set_visible(bool v) noexcept { visible_ = v; }
  void toggle() noexcept { visible_ = !visible_; }
  [[nodiscard]] bool visible() const noexcept { return visible_; }

  // The overlay's bounding rectangle in logical pixels (fixed, top-left).
  // Used by the SHM example to damage exactly the HUD region each frame.
  [[nodiscard]] static SkIRect Bounds() noexcept {
    return SkIRect::MakeXYWH(kOriginX, kOriginY, kWidth, kHeight);
  }

  // Draw the overlay onto canvas in logical pixels.  No-op when hidden.  fps is
  // the current rate (from FpsMeter); the rest is read from pacer.
  void Render(SkCanvas* canvas, const FramePacer& pacer, double fps);

 private:
  static constexpr int kOriginX = 8;
  static constexpr int kOriginY = 8;
  static constexpr int kWidth = 236;
  static constexpr int kPad = 6;
  static constexpr int kLineH = 17;
  static constexpr int kLines = 3;
  static constexpr int kHeight = kPad * 2 + kLineH * kLines;
  static constexpr float kFontSize = 14.0F;

  void EnsureFont();

  bool visible_ = false;
  bool font_ready_ = false;
  SkFont font_;
};

}  // namespace demo
