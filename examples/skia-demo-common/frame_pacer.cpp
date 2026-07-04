// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors

#include "frame_pacer.hpp"

#include <algorithm>
#include <cmath>

namespace demo {

namespace {

double MeanOf(const std::vector<double>& v) noexcept {
  if (v.empty())
    return 0.0;
  double sum = 0.0;
  for (const double d : v)
    sum += d;
  return sum / static_cast<double>(v.size());
}

// Nearest-rank percentile: rank = ceil(p/100 * N), 1-based, clamped to [1, N].
double PercentileOf(const std::vector<double>& v, double p) {
  if (v.empty())
    return 0.0;
  std::vector<double> sorted = v;
  std::sort(sorted.begin(), sorted.end());
  const double clamped = std::min(std::max(p, 0.0), 100.0);
  const auto n = static_cast<double>(sorted.size());
  auto rank = static_cast<std::size_t>(std::ceil(clamped / 100.0 * n));
  if (rank == 0)
    rank = 1;
  if (rank > sorted.size())
    rank = sorted.size();
  return sorted[rank - 1];
}

}  // namespace

double FramePacer::Mean() const noexcept {
  return MeanOf(durations_);
}

double FramePacer::Percentile(double p) const noexcept {
  return PercentileOf(durations_, p);
}

double FramePacer::PresentMean() const noexcept {
  return MeanOf(present_ms_);
}

double FramePacer::PresentPercentile(double p) const noexcept {
  return PercentileOf(present_ms_, p);
}

}  // namespace demo
