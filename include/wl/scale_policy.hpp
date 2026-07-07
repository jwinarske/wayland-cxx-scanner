// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// ScalePolicy — the normative buffer-at-physical / viewport-to-logical scaling
// policy for downstream consumers: the buffer is allocated at physical pixels,
// the wp_viewport destination is the logical size, and the scene renders in
// logical units through a single top-level canvas scale.
//
// Fractional scale is carried as `scale_120`, the scale in 1/120 units,
// exactly as wp_fractional_scale_v1.preferred_scale delivers it (unity is
// 120). Integer wl_output.scale N maps to scale_120 = N * 120.
//
// Rounding is done in the integer domain — round-half-up of
// `logical * scale_120 / 120` — NOT as `lround(logical * scale_120 / 120.0)`.
// The two agree almost everywhere but disagree by one pixel wherever the
// floating-point product lands just off an exact half (e.g. 100 px at 1.025x:
// 100*123/120 = 102.5 -> 103, but the double 123/120.0 * 100 is 102.4999... ->
// lround 102). One pixel is a real seam on a fractional-scaled panel, so this
// is the single normative rounding; the conformance vectors in
// <wl/scale_policy_vectors.hpp> are the shared contract that downstream copies
// (which reimplement the policy rather than link this header — e.g. builds
// with no Wayland at all) test against so they cannot diverge.
//
// Header-only, depends only on <cstdint>.

#pragma once

#include <cstdint>

namespace wl {

class ScalePolicy {
 public:
  // wp_fractional_scale_v1 expresses unity scale as 120.
  static constexpr std::int32_t kUnityScale120 = 120;

  struct BufferSize {
    std::int32_t width = 0;
    std::int32_t height = 0;
  };

  // Physical buffer dimension for a logical dimension at `scale_120`, rounded
  // to the nearest whole pixel (half up) in the integer domain. A non-positive
  // dimension yields 0; a non-positive `scale_120` is treated as unity so a
  // missing advertisement degrades to integer scale 1.
  [[nodiscard]] static constexpr std::int32_t ScaledDim(
      const std::int32_t logical,
      const std::int32_t scale_120) noexcept {
    if (logical <= 0) {
      return 0;
    }
    const std::int32_t s = Normalize(scale_120);
    // 64-bit intermediate so a logical size near INT32_MAX cannot overflow the
    // product before the divide.
    const std::int64_t num = static_cast<std::int64_t>(logical) * s;
    return static_cast<std::int32_t>((num + kUnityScale120 / 2) /
                                     kUnityScale120);
  }

  // Physical buffer size for a logical size at `scale_120`.
  [[nodiscard]] static constexpr BufferSize ToBuffer(
      const std::int32_t logical_w,
      const std::int32_t logical_h,
      const std::int32_t scale_120) noexcept {
    return BufferSize{ScaledDim(logical_w, scale_120),
                      ScaledDim(logical_h, scale_120)};
  }

  // Canvas scale factor (logical -> buffer pixels) as a double, for the
  // FP-domain consumers (top-level canvas transform, pointer scale) that do
  // not need pixel-exact rounding.
  [[nodiscard]] static constexpr double CanvasScale(
      const std::int32_t scale_120) noexcept {
    return static_cast<double>(Normalize(scale_120)) /
           static_cast<double>(kUnityScale120);
  }

 private:
  [[nodiscard]] static constexpr std::int32_t Normalize(
      const std::int32_t scale_120) noexcept {
    return scale_120 > 0 ? scale_120 : kUnityScale120;
  }
};

}  // namespace wl
