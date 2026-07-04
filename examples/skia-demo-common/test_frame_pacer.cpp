// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Unit tests for the frame pacer.

#include "frame_pacer.hpp"

#include <gtest/gtest.h>

using demo::FramePacer;
using demo::PacerConfig;

namespace {

TEST(FramePacer, UnboundedNeverReachesLimit) {
  FramePacer pacer{PacerConfig{}};
  EXPECT_FALSE(pacer.self_paced());
  for (int i = 0; i < 1000; ++i) {
    EXPECT_FALSE(pacer.reached_limit());
    pacer.Advance();
  }
}

TEST(FramePacer, FrameLimitStopsAtCount) {
  FramePacer pacer{PacerConfig{/*max_frames=*/3, /*exit_on_limit=*/true}};
  EXPECT_TRUE(pacer.self_paced());
  EXPECT_EQ(pacer.frame(), 0u);
  EXPECT_FALSE(pacer.reached_limit());
  pacer.Advance();
  pacer.Advance();
  EXPECT_FALSE(pacer.reached_limit());
  pacer.Advance();
  EXPECT_TRUE(pacer.reached_limit());
  EXPECT_EQ(pacer.frame(), 3u);
}

TEST(FramePacer, LimitWithoutExitIsNotSelfPaced) {
  FramePacer pacer{PacerConfig{/*max_frames=*/10, /*exit_on_limit=*/false}};
  EXPECT_FALSE(pacer.self_paced());
}

TEST(FramePacer, BenchmarkIsSelfPaced) {
  PacerConfig cfg;
  cfg.benchmark = true;
  FramePacer pacer{cfg};
  EXPECT_TRUE(pacer.self_paced());
  EXPECT_TRUE(pacer.benchmarking());
}

TEST(FramePacer, RealClockPassesThrough) {
  FramePacer pacer{PacerConfig{}};
  EXPECT_DOUBLE_EQ(pacer.AnimationTimeMs(1234), 1234.0);
}

TEST(FramePacer, FixedClockIsDeterministicPerFrame) {
  PacerConfig cfg;
  cfg.fixed_dt = true;
  FramePacer pacer{cfg};
  EXPECT_DOUBLE_EQ(pacer.AnimationTimeMs(9999), 0.0);
  pacer.Advance();
  EXPECT_DOUBLE_EQ(pacer.AnimationTimeMs(9999), FramePacer::kFixedStepMs);
  pacer.Advance();
  EXPECT_DOUBLE_EQ(pacer.AnimationTimeMs(0), 2.0 * FramePacer::kFixedStepMs);
}

TEST(FramePacer, StatsEmptyAreZero) {
  FramePacer pacer{PacerConfig{}};
  EXPECT_EQ(pacer.sample_count(), 0u);
  EXPECT_DOUBLE_EQ(pacer.Mean(), 0.0);
  EXPECT_DOUBLE_EQ(pacer.Percentile(95), 0.0);
}

TEST(FramePacer, MeanAndPercentiles) {
  FramePacer pacer{PacerConfig{}};
  // 1..10 ms.
  for (int i = 1; i <= 10; ++i)
    pacer.RecordFrameMs(static_cast<double>(i));
  EXPECT_EQ(pacer.sample_count(), 10u);
  EXPECT_DOUBLE_EQ(pacer.Mean(), 5.5);
  // Nearest-rank: p50 -> rank ceil(0.5*10)=5 -> value 5; p95 -> rank 10 -> 10.
  EXPECT_DOUBLE_EQ(pacer.Percentile(50), 5.0);
  EXPECT_DOUBLE_EQ(pacer.Percentile(95), 10.0);
  EXPECT_DOUBLE_EQ(pacer.Percentile(0), 1.0);
  EXPECT_DOUBLE_EQ(pacer.Percentile(100), 10.0);
}

}  // namespace
