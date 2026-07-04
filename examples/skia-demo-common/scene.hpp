// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// DemoScene — the single shared scene rendered by every Skia example so their
// output is directly comparable.  It draws through a plain SkCanvas and knows
// nothing about the buffer path underneath (wl_shm raster, GL, Vulkan), which
// is what makes a differential golden-image harness possible.

#pragma once

#include <cstdint>

class SkCanvas;
struct SkIRect;

namespace demo {

// Immutable per-frame inputs to the scene.  Coordinates are logical pixels;
// the caller applies any fractional-scale transform on the canvas before
// calling Render (see scale.hpp).
struct SceneState {
  int width = 0;               // logical width  in pixels
  int height = 0;              // logical height in pixels
  std::uint32_t frame = 0;     // monotone frame counter; drives animation
  bool button_active = false;  // true while the button is held
};

class DemoScene {
 public:
  // Draws the scene into `canvas` in logical coordinates.  When `out_damage`
  // is non-null it receives the logical-pixel bounds that changed this frame;
  // the caller maps that to buffer pixels and to wl_surface.damage_buffer.
  static void Render(SkCanvas* canvas,
                     const SceneState& state,
                     SkIRect* out_damage) noexcept;
};

}  // namespace demo
