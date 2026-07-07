// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Conformance vectors for ScalePolicy — the shared, canonical table that every
// implementation of the buffer-at-physical rounding must reproduce.
//
// wayland-cxx-scanner is the reference (see <wl/scale_policy.hpp>). Downstream
// consumers that cannot link this header — e.g. an embedder whose DRM/KMS or
// software backend links no Wayland at all — reimplement the policy and must
// vendor a byte-identical copy of THIS file and assert their implementation
// against `kToBufferVectors` in CI. Keep the two copies in lock-step: any edit
// here is an edit to the contract.
//
// Self-contained: depends only on <cstdint>, references nothing from
// scale_policy.hpp, so a copy can be dropped into any project's tests as-is.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wl::scale_conformance {

// logical (w,h) at scale_120 must produce buffer (expect_w, expect_h).
struct ToBufferVector {
  std::int32_t logical_w;
  std::int32_t logical_h;
  std::int32_t scale_120;
  std::int32_t expect_w;
  std::int32_t expect_h;
};

inline constexpr ToBufferVector kToBufferVectors[] = {
    // Unity and integral fractions.
    {480, 320, 120, 480, 320},      // 1.0
    {480, 320, 150, 600, 400},      // 1.25
    {480, 320, 180, 720, 480},      // 1.5
    {480, 320, 240, 960, 640},      // 2.0
    {1000, 1000, 360, 3000, 3000},  // 3.0
    // Round-half-up boundaries. The half cases (123, 990@122, 60@123) are
    // exactly where a floating-point `lround(logical * scale/120.0)` rounds
    // down one pixel short; the policy rounds up, per spec.
    {100, 100, 123, 103, 103},    // 102.5   -> 103  (FP form gave 102)
    {100, 100, 127, 106, 106},    // 105.83  -> 106
    {100, 100, 122, 102, 102},    // 101.67  -> 102
    {100, 100, 126, 105, 105},    // 105.0   -> 105  (exact)
    {60, 60, 123, 62, 62},        // 61.5    -> 62   (FP form gave 61)
    {990, 990, 122, 1007, 1007},  // 1006.5  -> 1007 (FP form gave 1006)
    // Degenerate inputs.
    {480, 320, 0, 480, 320},          // non-positive scale -> unity
    {480, 320, -10, 480, 320},        // negative scale -> unity
    {0, 0, 180, 0, 0},                // zero logical stays zero
    {4096, 4096, 360, 12288, 12288},  // large, 64-bit intermediate, no overflow
};

inline constexpr std::size_t kToBufferVectorCount =
    sizeof(kToBufferVectors) / sizeof(kToBufferVectors[0]);

// scale_120 must produce this canvas scale factor.
struct CanvasScaleVector {
  std::int32_t scale_120;
  double expect;
};

inline constexpr CanvasScaleVector kCanvasScaleVectors[] = {
    {120, 1.0}, {180, 1.5}, {240, 2.0}, {0, 1.0},  // non-positive -> unity
    {-5, 1.0},
};

inline constexpr std::size_t kCanvasScaleVectorCount =
    sizeof(kCanvasScaleVectors) / sizeof(kCanvasScaleVectors[0]);

}  // namespace wl::scale_conformance
