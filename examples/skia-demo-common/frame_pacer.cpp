// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors

#include "frame_pacer.hpp"

#include <algorithm>
#include <cmath>

namespace demo {

double FramePacer::Mean() const noexcept {
  if (durations_.empty())
    return 0.0;
  double sum = 0.0;
  for (const double d : durations_)
    sum += d;
  return sum / static_cast<double>(durations_.size());
}

double FramePacer::Percentile(double p) const noexcept {
  if (durations_.empty())
    return 0.0;
  std::vector<double> sorted = durations_;
  std::sort(sorted.begin(), sorted.end());
  const double clamped = std::min(std::max(p, 0.0), 100.0);
  // Nearest-rank: rank = ceil(p/100 * N), 1-based, clamped to [1, N].
  const auto n = static_cast<double>(sorted.size());
  auto rank = static_cast<std::size_t>(std::ceil(clamped / 100.0 * n));
  if (rank == 0)
    rank = 1;
  if (rank > sorted.size())
    rank = sorted.size();
  return sorted[rank - 1];
}

}  // namespace demo
