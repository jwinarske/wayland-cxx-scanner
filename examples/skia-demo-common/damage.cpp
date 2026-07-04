// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors

#include "damage.hpp"

namespace demo {

namespace {

// True when the two rectangles overlap or share an edge, so their union does
// not enclose any pixels neither of them damaged.
bool Touches(const SkIRect& a, const SkIRect& b) noexcept {
  return a.fLeft <= b.fRight && b.fLeft <= a.fRight && a.fTop <= b.fBottom &&
         b.fTop <= a.fBottom;
}

}  // namespace

void ClampToBounds(std::vector<SkIRect>& rects,
                   const SkIRect& bounds) noexcept {
  auto out = rects.begin();
  for (const SkIRect& r : rects) {
    SkIRect clipped = r;
    if (clipped.intersect(bounds))
      *out++ = clipped;
  }
  rects.erase(out, rects.end());
}

void Coalesce(std::vector<SkIRect>& rects) noexcept {
  bool merged = true;
  while (merged) {
    merged = false;
    for (std::size_t i = 0; i < rects.size(); ++i) {
      for (std::size_t j = i + 1; j < rects.size();) {
        if (Touches(rects[i], rects[j])) {
          rects[i].join(rects[j]);
          rects[j] = rects.back();
          rects.pop_back();
          merged = true;
        } else {
          ++j;
        }
      }
    }
  }
}

}  // namespace demo
