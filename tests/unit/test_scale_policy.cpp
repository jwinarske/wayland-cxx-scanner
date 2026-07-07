// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// ScalePolicy conformance. Drives the shared vector table
// (<wl/scale_policy_vectors.hpp>) — the same table downstream copies of the
// policy must reproduce — plus a regression pinning the round-half-up domain
// against the floating-point form it replaces.

#include <cmath>

#include <gtest/gtest.h>

#include <wl/scale_policy.hpp>
#include <wl/scale_policy_vectors.hpp>

using wl::ScalePolicy;
namespace vec = wl::scale_conformance;

TEST(ScalePolicy, ToBufferMatchesSharedVectors) {
  for (const auto& v : vec::kToBufferVectors) {
    const auto b = ScalePolicy::ToBuffer(v.logical_w, v.logical_h, v.scale_120);
    EXPECT_EQ(b.width, v.expect_w)
        << "logical_w=" << v.logical_w << " scale_120=" << v.scale_120;
    EXPECT_EQ(b.height, v.expect_h)
        << "logical_h=" << v.logical_h << " scale_120=" << v.scale_120;
  }
}

TEST(ScalePolicy, CanvasScaleMatchesSharedVectors) {
  for (const auto& v : vec::kCanvasScaleVectors) {
    EXPECT_DOUBLE_EQ(ScalePolicy::CanvasScale(v.scale_120), v.expect)
        << "scale_120=" << v.scale_120;
  }
}

TEST(ScalePolicy, UnityConstant) {
  EXPECT_EQ(ScalePolicy::kUnityScale120, 120);
}

// The policy replaced std::lround(dim * (scale_120 / 120.0)). At exact halves
// the double product lands just under .5 and truncates down, one pixel short.
// Assert the divergence is real (so the vectors above are not a tautology).
TEST(ScalePolicy, RoundHalfUpDivergesFromFloatingPointForm) {
  struct Case {
    std::int32_t dim, scale_120;
  };
  constexpr Case kHalves[] = {{100, 123}, {60, 123}, {990, 122}};
  for (const auto& c : kHalves) {
    const auto policy = ScalePolicy::ScaledDim(c.dim, c.scale_120);
    const auto fp = static_cast<std::int32_t>(
        std::lround(c.dim * (static_cast<double>(c.scale_120) / 120.0)));
    EXPECT_EQ(fp, policy - 1)
        << "expected the fp form one pixel short at dim=" << c.dim
        << " scale_120=" << c.scale_120;
  }
}
