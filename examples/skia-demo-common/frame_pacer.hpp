// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// FramePacer — frame counting, run limits, a deterministic animation clock, and
// frame-time statistics, shared by the Skia examples.
//
// It is pure logic (no Wayland, no Skia): the example owns the render loop and
// feeds real per-frame durations in.  This keeps it unit-testable and lets the
// same policy drive the interactive frame-callback loop and the bounded,
// self-paced loop used for headless runs and benchmarks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace demo {

struct PacerConfig {
  int max_frames = 0;          // stop after this many frames (0 = unbounded)
  bool exit_on_limit = false;  // quit once the frame limit is reached
  bool benchmark = false;      // self-paced run collecting frame-time stats
  bool fixed_dt = false;       // deterministic 60 Hz animation clock
};

class FramePacer {
 public:
  // Synthetic timestep used when fixed_dt is set (60 Hz).
  static constexpr double kFixedStepMs = 1000.0 / 60.0;

  explicit FramePacer(PacerConfig cfg) noexcept : cfg_(cfg) {}

  [[nodiscard]] std::uint32_t frame() const noexcept { return frame_; }
  [[nodiscard]] bool benchmarking() const noexcept { return cfg_.benchmark; }

  // A bounded run (benchmark, or an explicit frame limit with --exit) is driven
  // by the example's self-paced loop rather than compositor frame callbacks, so
  // it runs to completion even when the surface is not visible.
  [[nodiscard]] bool self_paced() const noexcept {
    return cfg_.benchmark || (cfg_.max_frames > 0 && cfg_.exit_on_limit);
  }

  [[nodiscard]] bool reached_limit() const noexcept {
    return cfg_.max_frames > 0 &&
           frame_ >= static_cast<std::uint32_t>(cfg_.max_frames);
  }

  // Animation clock in milliseconds.  With fixed_dt a given frame index always
  // maps to the same time (reproducible animation); otherwise the caller's real
  // timestamp passes through.
  [[nodiscard]] double AnimationTimeMs(
      std::uint32_t real_time_ms) const noexcept {
    return cfg_.fixed_dt ? static_cast<double>(frame_) * kFixedStepMs
                         : static_cast<double>(real_time_ms);
  }

  void RecordFrameMs(double ms) { durations_.push_back(ms); }
  void Advance() noexcept { ++frame_; }

  [[nodiscard]] std::size_t sample_count() const noexcept {
    return durations_.size();
  }
  [[nodiscard]] double Mean() const noexcept;
  // Nearest-rank percentile of recorded frame times; p in [0, 100].
  [[nodiscard]] double Percentile(double p) const noexcept;

  // ── Presentation-feedback stats (wp_presentation) ──────────────────────────
  // Optional: populated only when the compositor supports wp_presentation and
  // the surface is actually presented.  Latency is commit→turn-to-light in ms.
  //
  // Unlike the frame-time samples (recorded only in a bounded self-paced run),
  // present latencies arrive continuously in the interactive loop, so they are
  // kept in a fixed-size ring — the last kPresentWindow samples — rather than
  // an unbounded vector.  present_count() still reports the lifetime total.

  static constexpr std::size_t kPresentWindow = 2048;

  void RecordPresentMs(double ms) {
    if (present_ms_.size() < kPresentWindow)
      present_ms_.push_back(ms);
    else
      present_ms_[present_next_] = ms;
    present_next_ = (present_next_ + 1) % kPresentWindow;
    ++present_total_;
  }
  // Remember the most recent plausible refresh interval (nanoseconds); 0 means
  // "unknown" and is ignored so a stale-but-valid value is retained.
  void NoteRefreshNs(std::uint32_t ns) noexcept {
    if (ns != 0)
      refresh_ns_ = ns;
  }

  // Lifetime count of presented frames (not the ring's current size).
  [[nodiscard]] std::uint64_t present_count() const noexcept {
    return present_total_;
  }
  [[nodiscard]] double PresentMean() const noexcept;
  // Nearest-rank percentile over the retained window; p in [0, 100].
  [[nodiscard]] double PresentPercentile(double p) const noexcept;
  // Measured refresh rate in Hz, or 0.0 when no refresh has been reported.
  [[nodiscard]] double refresh_hz() const noexcept {
    return refresh_ns_ != 0 ? 1.0e9 / static_cast<double>(refresh_ns_) : 0.0;
  }
  // Measured refresh interval in nanoseconds, or 0 when no refresh has been
  // reported.  Lets the render loop retune its production timer to the display.
  [[nodiscard]] std::uint32_t refresh_ns() const noexcept {
    return refresh_ns_;
  }

 private:
  PacerConfig cfg_;
  std::uint32_t frame_ = 0;
  std::vector<double> durations_;
  std::vector<double> present_ms_;   // ring of the last kPresentWindow samples
  std::size_t present_next_ = 0;     // next write index once the ring is full
  std::uint64_t present_total_ = 0;  // lifetime count of presented frames
  std::uint32_t refresh_ns_ = 0;
};

}  // namespace demo
