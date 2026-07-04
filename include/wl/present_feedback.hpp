// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// present_feedback — the typed result of a wp_presentation_feedback.presented
// event, plus the pure decode of that event's raw wire fields.  Split out from
// presentation.hpp so it carries no dependency on the generated protocol types
// and can be unit-tested (and reused) on its own.

#pragma once

#include <cstdint>
#include <optional>

namespace wl {

// One presentation-feedback result for a committed frame.  Carries the raw
// wp_presentation_feedback.presented fields — so a consumer can phase-lock a
// scheduler on the absolute present timestamp — plus the commit→photons latency
// derived from them.
struct PresentFeedback {
  std::uint32_t frame = 0;       // App frame index recorded at commit
  std::uint64_t present_ns = 0;  // presentation-clock time the frame lit up
  std::uint32_t refresh_ns = 0;  // reported refresh interval (0 if implausible)
  std::uint64_t seq = 0;         // output vblank sequence counter (msc)
  std::uint32_t flags = 0;       // WP_PRESENTATION_FEEDBACK_KIND_* bitmask
  double latency_ms = 0.0;       // present_ns − commit_ns, in milliseconds
};

namespace detail {

// Decode a wp_presentation_feedback.presented event into a PresentFeedback,
// applying two guards learned from field implementations:
//
//   * tv_sec_hi must be zero — a nonzero seconds-high word would overflow the
//     ×1e9 below (a CLOCK_MONOTONIC seconds-since-boot count has never
//     approached 2^32, ~136 years).  A nonzero value returns nullopt so the
//     caller treats the frame as discarded rather than reporting a garbage
//     timestamp.
//   * refresh outside [min_refresh_ns, max_refresh_ns] is reported as 0
//     ("unknown") rather than trusted, clamping a virtual or hostile
//     compositor's absurd value.
//
// @param commit_ns  Commit time on the presentation clock (same domain as the
//                   reported timestamp) used to derive latency_ms.
[[nodiscard]] inline std::optional<PresentFeedback> DecodePresented(
    std::uint32_t frame,
    double commit_ns,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec,
    std::uint32_t refresh,
    std::uint32_t seq_hi,
    std::uint32_t seq_lo,
    std::uint32_t flags,
    std::uint32_t min_refresh_ns,
    std::uint32_t max_refresh_ns) noexcept {
  if (tv_sec_hi != 0)
    return std::nullopt;
  const std::uint64_t present_ns =
      static_cast<std::uint64_t>(tv_sec_lo) * 1'000'000'000ull + tv_nsec;
  const std::uint32_t safe_refresh =
      (refresh >= min_refresh_ns && refresh <= max_refresh_ns) ? refresh : 0u;
  const double latency_ms =
      (static_cast<double>(present_ns) - commit_ns) / 1.0e6;
  return PresentFeedback{
      frame,        present_ns,
      safe_refresh, (static_cast<std::uint64_t>(seq_hi) << 32) | seq_lo,
      flags,        latency_ms};
}

}  // namespace detail
}  // namespace wl
