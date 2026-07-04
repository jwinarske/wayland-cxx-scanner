// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// DemoScene — the single shared scene rendered by every Skia example so their
// output is directly comparable.  It draws through a plain SkCanvas and knows
// nothing about the buffer path underneath (wl_shm raster, GL, Vulkan), which
// is what makes a differential golden-image harness possible.
//
// View geometry comes from a ViewTree (see view_tree.hpp): the scene draws each
// view at its bounds, and the caller uses the same tree for hit-testing and
// damage.

#pragma once

#include <cstdint>

class SkCanvas;

namespace demo {

class ViewTree;

// Immutable per-frame inputs to the scene.  The caller applies any
// fractional-scale transform on the canvas before calling Render (see
// scale.hpp).
struct SceneState {
  std::uint32_t frame = 0;     // monotone frame counter; drives animation
  bool button_active = false;  // true while the button is held
  // Draw the text card's glyphs.  Off for golden-image tests, whose fonts come
  // from the system and so are not reproducible; the rest of the scene is.
  bool draw_text = true;
};

class DemoScene {
 public:
  // Draws the scene into `canvas` in logical coordinates, placing each view at
  // its bounds in `views`.
  static void Render(SkCanvas* canvas,
                     const SceneState& state,
                     const ViewTree& views) noexcept;
};

}  // namespace demo
