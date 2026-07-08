// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Unit tests for FpsMeter's rolling-window logic and PerfHud's visibility
// state.  The Skia rendering path is exercised by the example, not here.

#include "perf_hud.hpp"

#include <gtest/gtest.h>

using demo::DamageMeter;
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

TEST(DamageMeter, EmptyIsZero) {
  DamageMeter m;
  EXPECT_DOUBLE_EQ(m.mean_fraction(), 0.0);
}

TEST(DamageMeter, AveragesSamplesInWindow) {
  DamageMeter m;
  // Three samples inside the window: mean of 0.2, 0.4, 0.6 = 0.4.
  m.Tick(1000.0, 0.2);
  m.Tick(1010.0, 0.4);
  m.Tick(1020.0, 0.6);
  EXPECT_NEAR(m.mean_fraction(), 0.4, 1e-9);
}

TEST(DamageMeter, EvictsSamplesOlderThanWindow) {
  DamageMeter m;
  m.Tick(1000.0, 1.0);  // will age out
  m.Tick(1010.0, 1.0);  // will age out
  // Jump forward past the window: only the latest sample remains.
  m.Tick(3000.0, 0.25);
  EXPECT_NEAR(m.mean_fraction(), 0.25, 1e-9);
}

TEST(DamageMeter, ClampsFractionToUnitRange) {
  DamageMeter m;
  m.Tick(1000.0, 5.0);   // clamps to 1.0
  m.Tick(1010.0, -3.0);  // clamps to 0.0
  EXPECT_NEAR(m.mean_fraction(), 0.5, 1e-9);
}

TEST(DamageMeter, DecaysToZeroWhenIdle) {
  DamageMeter m;
  for (int i = 0; i < 60; ++i)
    m.Tick(1000.0 + static_cast<double>(i) * (1000.0 / 60.0), 0.5);
  EXPECT_NEAR(m.mean_fraction(), 0.5, 1e-9);
  // A held animation stops ticking; the next observation a window later reads
  // 0.
  DamageMeter idle = m;
  // No further Ticks — mean stays until samples would be evicted by a new Tick.
  // Model a resumed tick well past the window to force eviction of the old
  // ones.
  idle.Tick(5000.0, 0.0);
  EXPECT_NEAR(idle.mean_fraction(), 0.0, 1e-9);
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
  PerfHud hud;
  const SkIRect a = hud.Bounds();
  const SkIRect b = hud.Bounds();
  EXPECT_EQ(a, b);
  EXPECT_GT(a.width(), 0);
  EXPECT_GT(a.height(), 0);
}

TEST(PerfHud, ExtraLinesGrowBounds) {
  PerfHud hud;
  const int base = hud.Bounds().height();

  hud.SetExtraLine(0, "commit  60/s");
  const int one = hud.Bounds().height();
  EXPECT_GT(one, base);

  hud.SetExtraLine(1, "damage  12.3%");
  const int two = hud.Bounds().height();
  EXPECT_GT(two, one);
  // Two equal-height rows added: each grew the panel by the same amount.
  EXPECT_EQ(two - one, one - base);

  // Clearing the last line shrinks it back; width is unaffected throughout.
  hud.SetExtraLine(1, nullptr);
  EXPECT_EQ(hud.Bounds().height(), one);
  EXPECT_EQ(hud.Bounds().width(), PerfHud().Bounds().width());
}

TEST(PerfHud, ExtraLineIndexOutOfRangeIsIgnored) {
  PerfHud hud;
  const int base = hud.Bounds().height();
  hud.SetExtraLine(PerfHud::kMaxExtraLines, "overflow");  // ignored
  EXPECT_EQ(hud.Bounds().height(), base);
}

}  // namespace
