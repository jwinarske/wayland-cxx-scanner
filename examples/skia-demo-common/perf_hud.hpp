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

// Rolling mean of a per-event fraction over a fixed time window — the same
// ring-buffer eviction as FpsMeter, but each retained sample carries a value in
// [0, 1] and the meter reports their average.  The Skottie example feeds it the
// per-commit damaged-area fraction, so the reported mean is the animation's
// average dirty coverage over the last second; an empty window reads 0.
class DamageMeter {
 public:
  static constexpr double kWindowMs = 1000.0;
  static constexpr std::size_t kCapacity = 240;

  // Record a sample at now_ms (any monotonic ms clock); fraction is clamped to
  // [0, 1].
  void Tick(double now_ms, double fraction) noexcept;

  // Mean of the samples still inside the window, or 0 when none remain.
  [[nodiscard]] double mean_fraction() const noexcept;

 private:
  std::array<double, kCapacity> stamps_{};
  std::array<double, kCapacity> fracs_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

// On-screen performance overlay.  Hidden by default; the example toggles it.
// Rendering is a no-op while hidden, so it costs nothing when off.
class PerfHud {
 public:
  void set_visible(bool v) noexcept { visible_ = v; }
  void toggle() noexcept { visible_ = !visible_; }
  [[nodiscard]] bool visible() const noexcept { return visible_; }

  // Up to this many application-specific lines may be appended under the
  // standard stats (e.g. the Skottie example's commit rate and damage
  // coverage).  Each non-empty line grows the panel by one row.
  static constexpr std::size_t kMaxExtraLines = 2;

  // Set (or clear, with nullptr/"") the extra line at idx.  A non-empty line is
  // drawn below the standard stats and included in Bounds(), so a consumer that
  // keeps a line populated gets a stable, larger panel to damage.
  void SetExtraLine(std::size_t idx, const char* text) noexcept;

  // The overlay's bounding rectangle in logical pixels (top-left origin).  Used
  // by the SHM/Skottie examples to damage exactly the HUD region each frame;
  // the height tracks how many extra lines are currently populated.
  [[nodiscard]] SkIRect Bounds() const noexcept {
    return SkIRect::MakeXYWH(kOriginX, kOriginY, kWidth, HeightPx());
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
  static constexpr std::size_t kExtraLineCap = 32;
  static constexpr float kFontSize = 14.0F;

  void EnsureFont();

  // Number of extra lines currently populated (drawn contiguously from idx 0).
  [[nodiscard]] std::size_t ActiveExtra() const noexcept;
  [[nodiscard]] int HeightPx() const noexcept {
    return kPad * 2 + kLineH * (kLines + static_cast<int>(ActiveExtra()));
  }

  bool visible_ = false;
  bool font_ready_ = false;
  SkFont font_;
  std::array<std::array<char, kExtraLineCap>, kMaxExtraLines> extra_{};
};

}  // namespace demo
