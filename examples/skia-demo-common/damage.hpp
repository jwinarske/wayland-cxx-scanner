// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Damage-list helpers operating on Skia integer rectangles.  Translating the
// resulting rectangles to wl_surface.damage_buffer stays in each example,
// where the wl_surface is in scope; this header is pure geometry so it is
// unit-testable without a compositor.

#pragma once

#include "include/core/SkRect.h"

#include <vector>

namespace demo {

// Clamps every rectangle to `bounds` and drops any that become empty.
void ClampToBounds(std::vector<SkIRect>& rects, const SkIRect& bounds) noexcept;

// Merges overlapping or edge-adjacent rectangles into their bounding union,
// repeating until no further merges are possible.  Reduces the number of
// damage_buffer calls without ever under-reporting the damaged region.
void Coalesce(std::vector<SkIRect>& rects) noexcept;

}  // namespace demo
