// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors

#include "scale.hpp"

namespace demo {

namespace {

// Round-half-up division by the fractional-scale denominator.
int ScaledRound(int logical, int scale_120) noexcept {
  if (logical <= 0)
    return 0;
  const long long num =
      static_cast<long long>(logical) * static_cast<long long>(scale_120);
  return static_cast<int>((num + ScalePolicy::kUnityScale120 / 2) /
                          ScalePolicy::kUnityScale120);
}

int NormalizeScale120(int scale_120) noexcept {
  return scale_120 > 0 ? scale_120 : ScalePolicy::kUnityScale120;
}

}  // namespace

BufferSize ScalePolicy::ToBuffer(int logical_w,
                                 int logical_h,
                                 int scale_120) noexcept {
  const int s = NormalizeScale120(scale_120);
  return BufferSize{ScaledRound(logical_w, s), ScaledRound(logical_h, s)};
}

double ScalePolicy::CanvasScale(int scale_120) noexcept {
  return static_cast<double>(NormalizeScale120(scale_120)) /
         static_cast<double>(kUnityScale120);
}

}  // namespace demo
