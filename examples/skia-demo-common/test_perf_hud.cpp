// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Unit tests for FpsMeter's rolling-window logic and PerfHud's visibility
// state.  The Skia rendering path is exercised by the example, not here.

#include "perf_hud.hpp"

#include <gtest/gtest.h>

using demo::FpsMeter;
using demo::PerfHud;

namespace {

TEST(FpsMeter, EmptyIsZero) {
  FpsMeter m;
  EXPECT_DOUBLE_EQ(m.fps(), 0.0);
}

TEST(FpsMeter, CountsFramesWithinWindow) {
  FpsMeter m;
  // 60 frames one 60 Hz interval apart, all within the last second.
  double t = 1000.0;
  for (int i = 0; i < 60; ++i) {
    m.Tick(t);
    t += 1000.0 / 60.0;
  }
  // The window is exactly 1 s, so the retained count is the FPS.
  EXPECT_GE(m.fps(), 59.0);
  EXPECT_LE(m.fps(), 60.0);
}

TEST(FpsMeter, EvictsSamplesOlderThanWindow) {
  FpsMeter m;
  // Fill a second's worth of frames…
  for (int i = 0; i < 60; ++i)
    m.Tick(1000.0 + static_cast<double>(i) * (1000.0 / 60.0));
  // …then jump forward 5 s and record one frame: all old samples age out.
  m.Tick(6000.0);
  EXPECT_DOUBLE_EQ(m.fps(), 1.0);
}

TEST(FpsMeter, SaturatesAtCapacity) {
  FpsMeter m;
  // Far more than kCapacity frames inside the window → saturates, no overflow.
  for (std::size_t i = 0; i < FpsMeter::kCapacity * 3; ++i)
    m.Tick(1000.0 + static_cast<double>(i) * 0.001);  // 1 kHz, all within 1 s
  EXPECT_DOUBLE_EQ(m.fps(), static_cast<double>(FpsMeter::kCapacity));
}

TEST(PerfHud, HiddenByDefault) {
  PerfHud hud;
  EXPECT_FALSE(hud.visible());
}

TEST(PerfHud, ToggleAndSetVisible) {
  PerfHud hud;
  hud.toggle();
  EXPECT_TRUE(hud.visible());
  hud.toggle();
  EXPECT_FALSE(hud.visible());
  hud.set_visible(true);
  EXPECT_TRUE(hud.visible());
}

TEST(PerfHud, BoundsAreNonEmptyAndStable) {
  const SkIRect a = PerfHud::Bounds();
  const SkIRect b = PerfHud::Bounds();
  EXPECT_EQ(a, b);
  EXPECT_GT(a.width(), 0);
  EXPECT_GT(a.height(), 0);
}

}  // namespace
