// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Unit tests for wl::detail::DecodePresented — the pure decode of a
// wp_presentation_feedback.presented event, including the overflow and
// refresh-plausibility guards.

#include <wl/present_feedback.hpp>

#include <cstdint>

#include <gtest/gtest.h>

namespace {

// Matches wl::PresentationManager's clamp window.
constexpr std::uint32_t kMinRefreshNs = 1'000'000u;    // 1000 Hz
constexpr std::uint32_t kMaxRefreshNs = 100'000'000u;  // 10 Hz

// One second, expressed on the presentation clock in nanoseconds.
constexpr double kOneSecondNs = 1'000'000'000.0;

std::optional<wl::PresentFeedback> Decode(std::uint32_t hi,
                                          std::uint32_t lo,
                                          std::uint32_t nsec,
                                          std::uint32_t refresh,
                                          std::uint32_t seq_hi = 0,
                                          std::uint32_t seq_lo = 0,
                                          std::uint32_t flags = 0,
                                          std::uint32_t frame = 0,
                                          double commit_ns = 0.0) {
  return wl::detail::DecodePresented(frame, commit_ns, hi, lo, nsec, refresh,
                                     seq_hi, seq_lo, flags, kMinRefreshNs,
                                     kMaxRefreshNs);
}

TEST(DecodePresented, ComputesLatencyFromCommitToPresent) {
  // Commit at t=1.000 s, presented at t=1.005 s → 5 ms latency.
  const auto fb = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/5'000'000u,
                         /*refresh=*/16'666'667u, /*seq_hi=*/0, /*seq_lo=*/0,
                         /*flags=*/0, /*frame=*/42,
                         /*commit_ns=*/kOneSecondNs);
  ASSERT_TRUE(fb.has_value());
  EXPECT_EQ(fb->frame, 42u);
  EXPECT_EQ(fb->present_ns, 1'005'000'000ull);
  EXPECT_EQ(fb->refresh_ns, 16'666'667u);
  EXPECT_DOUBLE_EQ(fb->latency_ms, 5.0);
}

TEST(DecodePresented, AssemblesPresentNsFromSecondsAndNanos) {
  const auto fb = Decode(/*hi=*/0, /*lo=*/7, /*nsec=*/123u,
                         /*refresh=*/16'666'667u);
  ASSERT_TRUE(fb.has_value());
  EXPECT_EQ(fb->present_ns, 7'000'000'123ull);
}

TEST(DecodePresented, CombinesSequenceHighAndLowWords) {
  const auto fb = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/0u,
                         /*refresh=*/16'666'667u, /*seq_hi=*/2u, /*seq_lo=*/3u);
  ASSERT_TRUE(fb.has_value());
  EXPECT_EQ(fb->seq, (static_cast<std::uint64_t>(2u) << 32) | 3u);
}

TEST(DecodePresented, PassesFlagsThrough) {
  const auto fb = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/0u,
                         /*refresh=*/16'666'667u, /*seq_hi=*/0, /*seq_lo=*/0,
                         /*flags=*/0x5u);
  ASSERT_TRUE(fb.has_value());
  EXPECT_EQ(fb->flags, 0x5u);
}

TEST(DecodePresented, NonzeroSecondsHighWordIsTreatedAsDiscard) {
  // A nonzero seconds-high word would overflow the ×1e9 math; decode must
  // return nullopt so the caller treats the frame as discarded.
  const auto fb = Decode(/*hi=*/1, /*lo=*/0, /*nsec=*/0u,
                         /*refresh=*/16'666'667u);
  EXPECT_FALSE(fb.has_value());
}

TEST(DecodePresented, ImplausiblyFastRefreshReportedAsUnknown) {
  // Below the 1000 Hz floor → reported as 0 ("unknown").
  const auto fb = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/0u,
                         /*refresh=*/kMinRefreshNs - 1u);
  ASSERT_TRUE(fb.has_value());
  EXPECT_EQ(fb->refresh_ns, 0u);
}

TEST(DecodePresented, ImplausiblySlowRefreshReportedAsUnknown) {
  // Above the 10 Hz ceiling → reported as 0 ("unknown").
  const auto fb = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/0u,
                         /*refresh=*/kMaxRefreshNs + 1u);
  ASSERT_TRUE(fb.has_value());
  EXPECT_EQ(fb->refresh_ns, 0u);
}

TEST(DecodePresented, ZeroRefreshReportedAsUnknown) {
  const auto fb = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/0u, /*refresh=*/0u);
  ASSERT_TRUE(fb.has_value());
  EXPECT_EQ(fb->refresh_ns, 0u);
}

TEST(DecodePresented, RefreshAtBoundariesIsKept) {
  const auto lo = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/0u,
                         /*refresh=*/kMinRefreshNs);
  const auto hi = Decode(/*hi=*/0, /*lo=*/1, /*nsec=*/0u,
                         /*refresh=*/kMaxRefreshNs);
  ASSERT_TRUE(lo.has_value());
  ASSERT_TRUE(hi.has_value());
  EXPECT_EQ(lo->refresh_ns, kMinRefreshNs);
  EXPECT_EQ(hi->refresh_ns, kMaxRefreshNs);
}

}  // namespace
