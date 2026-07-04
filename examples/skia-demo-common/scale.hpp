// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Scale policy shared by every Skia example, and the normative reference for
// downstream consumers: the buffer is allocated at physical pixels, the
// viewport destination is set to the logical size, and the scene renders in
// logical units through a single top-level canvas scale.
//
// Fractional scale is carried as `scale_120`, the compositor's scale in
// 1/120 units, exactly as wp_fractional_scale_v1 delivers it.

#pragma once

namespace demo {

struct BufferSize {
  int width = 0;
  int height = 0;
};

class ScalePolicy {
 public:
  // wp_fractional_scale_v1 expresses unity scale as 120.
  static constexpr int kUnityScale120 = 120;

  // Physical buffer size for a logical size at `scale_120`, rounded to the
  // nearest whole pixel per the fractional-scale specification.  A
  // non-positive `scale_120` is treated as unity so a missing advertisement
  // degrades to integer scale 1.
  static BufferSize ToBuffer(int logical_w,
                             int logical_h,
                             int scale_120) noexcept;

  // Canvas scale factor mapping logical units to buffer pixels.
  static double CanvasScale(int scale_120) noexcept;
};

}  // namespace demo
