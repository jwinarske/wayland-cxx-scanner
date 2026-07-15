// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_fallback — Header-only fallback CSD plugin using flat-color SHM
// rendering.
//
// This is the "regular" decoration plugin that requires no external
// dependencies.  It draws a dark title bar with close/maximize/minimize
// buttons (solid-color rectangles) and thin resize borders around the
// content area.
//
// ── Include order
// ───────────────────────────────────────────────────────────── Include
// <wl/csd_plugin.hpp> before this header.
//
//   #include <wl/csd_plugin.hpp>
//   #include <wl/csd_fallback.hpp>
#pragma once

#include <wl/csd_plugin.hpp>
#include <wl/scale_policy.hpp>

#include <cstdint>
#include <string>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// FallbackCsdPlugin — flat-color SHM CSD plugin
// ══════════════════════════════════════════════════════════════════════════════

/// Draws client-side decorations using solid-color rectangles: a dark
/// title bar (with close/maximize/minimize buttons), and thin resize
/// borders.  No external rendering library is needed.
class FallbackCsdPlugin final : public CsdPlugin {
 public:
  // ── Default color theme (ARGB8888, premultiplied — all opaque) ─────────
  static constexpr uint32_t kColorTitleBar = 0xFF3C3C3C;
  static constexpr uint32_t kColorTitleBarUnfocused = 0xFF505050;
  static constexpr uint32_t kColorBorder = 0xFF505050;
  static constexpr uint32_t kColorCloseBtn = 0xFFE04040;
  static constexpr uint32_t kColorMaxBtn = 0xFF40A040;
  static constexpr uint32_t kColorMinBtn = 0xFFD0A020;

  static constexpr int kBorderWidth = 4;
  static constexpr int kTitleBarHeight = 30;
  static constexpr int kButtonSize = 18;
  static constexpr int kButtonPadding = 6;

  // ── CsdPlugin interface ────────────────────────────────────────────────

  [[nodiscard]] Margins DecorationMargins() const override {
    return {kBorderWidth, kBorderWidth, kTitleBarHeight, kBorderWidth};
  }

  void SetTitle(std::string_view title) override { title_ = title; }

  void SetInputState(const InputState& state) override { state_ = state; }

  void SetScale(int scale_120) override { scale_120_ = scale_120; }

  void RenderDecoration(uint32_t* buffer,
                        int stride_px,
                        int surface_w,
                        int surface_h,
                        int content_w,
                        int /*content_h*/) override {
    // Everything arrives logical and the buffer is physical, so every
    // dimension is scaled on the way in. Flat color has no detail to gain
    // from the extra pixels, but it still has to fill them: drawn at logical
    // size it would occupy a corner of the buffer.
    const int bw = S(kBorderWidth);
    const int tbh = S(kTitleBarHeight);
    const int btn = S(kButtonSize);
    const int pad = S(kButtonPadding);
    const int sw = S(surface_w);
    const int sh = S(surface_h);
    const int cw = S(content_w);

    // Borders — the four bands around the content rect.  The content area is
    // left untouched for the application to paint.
    FillRect(buffer, stride_px, 0, 0, sw, tbh, kColorBorder);
    FillRect(buffer, stride_px, 0, sh - bw, sw, bw, kColorBorder);
    FillRect(buffer, stride_px, 0, tbh, bw, sh - tbh - bw, kColorBorder);
    FillRect(buffer, stride_px, sw - bw, tbh, bw, sh - tbh - bw, kColorBorder);

    // Title bar.
    const uint32_t tb_color =
        state_.focused ? kColorTitleBar : kColorTitleBarUnfocused;
    FillRect(buffer, stride_px, bw, bw, cw, tbh - bw, tb_color);

    // Close button (top-right of title bar).
    const int btn_y = bw + (tbh - bw - btn) / 2;
    int btn_x = bw + cw - pad - btn;
    FillRect(buffer, stride_px, btn_x, btn_y, btn, btn, kColorCloseBtn);

    // Maximize button.
    btn_x -= (btn + pad);
    FillRect(buffer, stride_px, btn_x, btn_y, btn, btn, kColorMaxBtn);

    // Minimize button.
    btn_x -= (btn + pad);
    FillRect(buffer, stride_px, btn_x, btn_y, btn, btn, kColorMinBtn);
  }

  [[nodiscard]] HitZone HitTest(int x,
                                int y,
                                int surface_w,
                                int surface_h,
                                int content_w,
                                int /*content_h*/) const noexcept override {
    // Outside surface bounds.
    if (x < 0 || y < 0 || x >= surface_w || y >= surface_h)
      return HitZone::None;

    // Corner resize zones (border × border squares at corners).
    if (x < kBorderWidth && y < kBorderWidth)
      return HitZone::ResizeTopLeft;
    if (x >= surface_w - kBorderWidth && y < kBorderWidth)
      return HitZone::ResizeTopRight;
    if (x < kBorderWidth && y >= surface_h - kBorderWidth)
      return HitZone::ResizeBottomLeft;
    if (x >= surface_w - kBorderWidth && y >= surface_h - kBorderWidth)
      return HitZone::ResizeBottomRight;

    // Edge resize zones.
    if (y < kBorderWidth)
      return HitZone::ResizeTop;
    if (y >= surface_h - kBorderWidth)
      return HitZone::ResizeBottom;
    if (x < kBorderWidth)
      return HitZone::ResizeLeft;
    if (x >= surface_w - kBorderWidth)
      return HitZone::ResizeRight;

    // Title bar region.
    if (y < kTitleBarHeight) {
      // Check buttons (right-aligned in title bar).
      const int btn_y =
          kBorderWidth + (kTitleBarHeight - kBorderWidth - kButtonSize) / 2;
      if (y >= btn_y && y < btn_y + kButtonSize) {
        int btn_x = kBorderWidth + content_w - kButtonPadding - kButtonSize;
        if (x >= btn_x && x < btn_x + kButtonSize)
          return HitZone::CloseButton;
        btn_x -= (kButtonSize + kButtonPadding);
        if (x >= btn_x && x < btn_x + kButtonSize)
          return HitZone::MaximizeButton;
        btn_x -= (kButtonSize + kButtonPadding);
        if (x >= btn_x && x < btn_x + kButtonSize)
          return HitZone::MinimizeButton;
      }
      return HitZone::TitleBar;
    }

    return HitZone::Content;
  }

 private:
  std::string title_;
  InputState state_;
  int scale_120_ = ScalePolicy::kUnityScale120;

  /// Logical to physical, using the project's normative rounding.
  [[nodiscard]] int S(int logical) const noexcept {
    return ScalePolicy::ScaledDim(logical, scale_120_);
  }

  // ── Pixel helpers ─────────────────────────────────────────────────────

  static void FillRect(uint32_t* buf,
                       int buf_w,
                       int x,
                       int y,
                       int w,
                       int h,
                       uint32_t color) noexcept {
    for (int row = y; row < y + h; ++row)
      for (int col = x; col < x + w; ++col)
        buf[row * buf_w + col] = color;
  }
};

}  // namespace wl::csd
