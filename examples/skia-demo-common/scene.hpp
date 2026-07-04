// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// DemoScene — the single shared scene rendered by every Skia example so their
// output is directly comparable.  It draws through a plain SkCanvas and knows
// nothing about the buffer path underneath (wl_shm raster, GL, Vulkan), which
// is what makes a differential golden-image harness possible.

#pragma once

#include <cstdint>
#include <vector>

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
  bool button_dirty = false;   // button changed since the last rendered frame
  // Draw the text card's glyphs.  Off for golden-image tests, whose fonts come
  // from the system and so are not reproducible; the rest of the scene is.
  bool draw_text = true;
};

class DemoScene {
 public:
  // Draws the scene into `canvas` in logical coordinates.  When `out_damage`
  // is non-null it is cleared and filled with the logical-pixel rectangles
  // that changed this frame (the animated spinner every frame, plus the button
  // when it toggled).  The caller maps these to buffer pixels and to
  // wl_surface.damage_buffer.
  static void Render(SkCanvas* canvas,
                     const SceneState& state,
                     std::vector<SkIRect>* out_damage) noexcept;
};

}  // namespace demo
