// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Unit tests for the damage-list helpers.

#include "damage.hpp"

#include "include/core/SkRect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

SkIRect R(int l, int t, int r, int b) {
  return SkIRect::MakeLTRB(l, t, r, b);
}

// Order-independent comparison: Coalesce may reorder rectangles.
void ExpectRects(std::vector<SkIRect> got, std::vector<SkIRect> want) {
  const auto by_corner = [](const SkIRect& a, const SkIRect& b) {
    return std::tie(a.fLeft, a.fTop, a.fRight, a.fBottom) <
           std::tie(b.fLeft, b.fTop, b.fRight, b.fBottom);
  };
  std::sort(got.begin(), got.end(), by_corner);
  std::sort(want.begin(), want.end(), by_corner);
  ASSERT_EQ(got.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i)
    EXPECT_EQ(got[i], want[i]) << "at index " << i;
}

// ── ClampToBounds
// ─────────────────────────────────────────────────────────────

TEST(ClampToBounds, EmptyStaysEmpty) {
  std::vector<SkIRect> rects;
  demo::ClampToBounds(rects, R(0, 0, 100, 100));
  EXPECT_TRUE(rects.empty());
}

TEST(ClampToBounds, RectFullyInsideIsUnchanged) {
  std::vector<SkIRect> rects = {R(10, 10, 40, 40)};
  demo::ClampToBounds(rects, R(0, 0, 100, 100));
  ExpectRects(rects, {R(10, 10, 40, 40)});
}

TEST(ClampToBounds, RectPartiallyOutsideIsClipped) {
  std::vector<SkIRect> rects = {R(-20, -20, 30, 30)};
  demo::ClampToBounds(rects, R(0, 0, 100, 100));
  ExpectRects(rects, {R(0, 0, 30, 30)});
}

TEST(ClampToBounds, RectFullyOutsideIsDropped) {
  std::vector<SkIRect> rects = {R(200, 200, 300, 300)};
  demo::ClampToBounds(rects, R(0, 0, 100, 100));
  EXPECT_TRUE(rects.empty());
}

TEST(ClampToBounds, MixedKeepsAndClipsAndDrops) {
  std::vector<SkIRect> rects = {
      R(10, 10, 20, 20),     // inside
      R(90, 90, 130, 130),   // clipped
      R(-50, -50, -10, -10)  // dropped
  };
  demo::ClampToBounds(rects, R(0, 0, 100, 100));
  ExpectRects(rects, {R(10, 10, 20, 20), R(90, 90, 100, 100)});
}

// ── Coalesce
// ──────────────────────────────────────────────────────────────────

TEST(Coalesce, EmptyStaysEmpty) {
  std::vector<SkIRect> rects;
  demo::Coalesce(rects);
  EXPECT_TRUE(rects.empty());
}

TEST(Coalesce, DisjointRectsAreKept) {
  // A one-pixel gap on the x axis: not touching.
  std::vector<SkIRect> rects = {R(0, 0, 10, 10), R(11, 0, 20, 10)};
  demo::Coalesce(rects);
  ExpectRects(rects, {R(0, 0, 10, 10), R(11, 0, 20, 10)});
}

TEST(Coalesce, OverlappingRectsMerge) {
  std::vector<SkIRect> rects = {R(0, 0, 20, 20), R(10, 10, 30, 30)};
  demo::Coalesce(rects);
  ExpectRects(rects, {R(0, 0, 30, 30)});
}

TEST(Coalesce, EdgeAdjacentRectsMerge) {
  // Right edge of the first equals the left of the second (SkIRect right is
  // exclusive), so the union covers no extra pixels.
  std::vector<SkIRect> rects = {R(0, 0, 10, 10), R(10, 0, 20, 10)};
  demo::Coalesce(rects);
  ExpectRects(rects, {R(0, 0, 20, 10)});
}

TEST(Coalesce, TransitiveChainMergesToOne) {
  // A touches B, B touches C, A does not touch C directly.
  std::vector<SkIRect> rects = {R(0, 0, 10, 10), R(9, 0, 20, 10),
                                R(19, 0, 30, 10)};
  demo::Coalesce(rects);
  ExpectRects(rects, {R(0, 0, 30, 10)});
}

TEST(Coalesce, IndependentGroupsStaySeparate) {
  std::vector<SkIRect> rects = {R(0, 0, 10, 10), R(5, 5, 15, 15),
                                R(100, 100, 110, 110)};
  demo::Coalesce(rects);
  ExpectRects(rects, {R(0, 0, 15, 15), R(100, 100, 110, 110)});
}

TEST(Coalesce, OrderIndependent) {
  std::vector<SkIRect> forward = {R(0, 0, 10, 10), R(9, 0, 20, 10),
                                  R(19, 0, 30, 10)};
  std::vector<SkIRect> reverse = {R(19, 0, 30, 10), R(9, 0, 20, 10),
                                  R(0, 0, 10, 10)};
  demo::Coalesce(forward);
  demo::Coalesce(reverse);
  ExpectRects(forward, {R(0, 0, 30, 10)});
  ExpectRects(reverse, {R(0, 0, 30, 10)});
}

}  // namespace
