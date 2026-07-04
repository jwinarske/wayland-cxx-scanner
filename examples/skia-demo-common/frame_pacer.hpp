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

 private:
  PacerConfig cfg_;
  std::uint32_t frame_ = 0;
  std::vector<double> durations_;
};

}  // namespace demo
