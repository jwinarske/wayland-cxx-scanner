// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Unit tests for the fractional-scale buffer-sizing policy.

#include "scale.hpp"

#include <gtest/gtest.h>

using demo::BufferSize;
using demo::ScalePolicy;

namespace {

// ── ToBuffer
// ──────────────────────────────────────────────────────────────────

TEST(ScalePolicy, UnityScaleIsIdentity) {
  const BufferSize b = ScalePolicy::ToBuffer(480, 320, 120);
  EXPECT_EQ(b.width, 480);
  EXPECT_EQ(b.height, 320);
}

TEST(ScalePolicy, IntegralFractions) {
  const BufferSize at125 = ScalePolicy::ToBuffer(480, 320, 150);  // 1.25
  EXPECT_EQ(at125.width, 600);
  EXPECT_EQ(at125.height, 400);

  const BufferSize at150 = ScalePolicy::ToBuffer(480, 320, 180);  // 1.5
  EXPECT_EQ(at150.width, 720);
  EXPECT_EQ(at150.height, 480);

  const BufferSize at2x = ScalePolicy::ToBuffer(480, 320, 240);  // 2.0
  EXPECT_EQ(at2x.width, 960);
  EXPECT_EQ(at2x.height, 640);
}

TEST(ScalePolicy, RoundsToNearestHalfUp) {
  // 100 * 123 / 120 = 102.5 -> 103 (round half up).
  EXPECT_EQ(ScalePolicy::ToBuffer(100, 100, 123).width, 103);
  // 100 * 127 / 120 = 105.83 -> 106.
  EXPECT_EQ(ScalePolicy::ToBuffer(100, 100, 127).width, 106);
  // 100 * 122 / 120 = 101.67 -> 102.
  EXPECT_EQ(ScalePolicy::ToBuffer(100, 100, 122).width, 102);
  // 100 * 126 / 120 = 105.0 -> 105 (exact).
  EXPECT_EQ(ScalePolicy::ToBuffer(100, 100, 126).width, 105);
}

TEST(ScalePolicy, NonPositiveScaleTreatedAsUnity) {
  EXPECT_EQ(ScalePolicy::ToBuffer(480, 320, 0).width, 480);
  EXPECT_EQ(ScalePolicy::ToBuffer(480, 320, 0).height, 320);
  EXPECT_EQ(ScalePolicy::ToBuffer(480, 320, -10).width, 480);
}

TEST(ScalePolicy, ZeroLogicalSizeStaysZero) {
  const BufferSize b = ScalePolicy::ToBuffer(0, 0, 180);
  EXPECT_EQ(b.width, 0);
  EXPECT_EQ(b.height, 0);
}

TEST(ScalePolicy, LargeSizesDoNotOverflow) {
  // 4096 * 360 / 120 = 12288, well within int range and computed in 64-bit.
  const BufferSize b = ScalePolicy::ToBuffer(4096, 4096, 360);
  EXPECT_EQ(b.width, 12288);
  EXPECT_EQ(b.height, 12288);
}

// ── CanvasScale
// ───────────────────────────────────────────────────────────────

TEST(ScalePolicy, CanvasScaleMatchesFraction) {
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(120), 1.0);
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(180), 1.5);
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(240), 2.0);
}

TEST(ScalePolicy, CanvasScaleNonPositiveIsUnity) {
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(0), 1.0);
  EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(-5), 1.0);
}

TEST(ScalePolicy, UnityConstant) {
  EXPECT_EQ(ScalePolicy::kUnityScale120, 120);
}

}  // namespace
