// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_fallback — Header-only fallback CSD plugin using flat-color SHM
// rendering.
//
// This is the "regular" decoration plugin that requires no external
// dependencies.  It draws a dark title bar with close/maximize/minimize
// buttons (solid-color rectangles) and thin resize borders around the
// content area — the same decoration style previously hard-coded in the
// xdg-csd example.
//
// ── Include order
// ───────────────────────────────────────────────────────────── Include
// <wl/csd_plugin.hpp> before this header.
//
//   #include <wl/csd_plugin.hpp>
//   #include <wl/csd_fallback.hpp>
#pragma once

#include <wl/csd_plugin.hpp>

#include <algorithm>
#include <cmath>
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
  // ── Default color theme (XRGB8888) ────────────────────────────────────
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

  [[nodiscard]] int BorderWidth() const noexcept override {
    return kBorderWidth;
  }

  [[nodiscard]] int TitleBarHeight() const noexcept override {
    return kTitleBarHeight;
  }

  void SetTitle(std::string_view title) override { title_ = title; }

  void SetState(bool focused, bool maximized) override {
    focused_ = focused;
    maximized_ = maximized;
  }

  void RenderFrame(uint32_t* buffer,
                   int surface_w,
                   int surface_h,
                   int content_w,
                   int content_h,
                   uint32_t time) override {
    // Fill entire surface with border color.
    FillRect(buffer, surface_w, 0, 0, surface_w, surface_h, kColorBorder);

    // Title bar.
    const uint32_t tb_color =
        focused_ ? kColorTitleBar : kColorTitleBarUnfocused;
    FillRect(buffer, surface_w, kBorderWidth, kBorderWidth, content_w,
             kTitleBarHeight - kBorderWidth, tb_color);

    // Close button (top-right of title bar).
    const int btn_y =
        kBorderWidth + (kTitleBarHeight - kBorderWidth - kButtonSize) / 2;
    int btn_x = kBorderWidth + content_w - kButtonPadding - kButtonSize;
    FillRect(buffer, surface_w, btn_x, btn_y, kButtonSize, kButtonSize,
             kColorCloseBtn);

    // Maximize button.
    btn_x -= (kButtonSize + kButtonPadding);
    FillRect(buffer, surface_w, btn_x, btn_y, kButtonSize, kButtonSize,
             kColorMaxBtn);

    // Minimize button.
    btn_x -= (kButtonSize + kButtonPadding);
    FillRect(buffer, surface_w, btn_x, btn_y, kButtonSize, kButtonSize,
             kColorMinBtn);

    // Content area — animated ring pattern.
    uint32_t* content_start =
        buffer + kTitleBarHeight * surface_w + kBorderWidth;
    PaintContent(content_start, content_w, content_h, surface_w, time);
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
  bool focused_ = true;
  bool maximized_ = false;

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

  static void PaintContent(uint32_t* pixels,
                           int width,
                           int height,
                           int stride,
                           uint32_t time) noexcept {
    const int halfh = height / 2;
    const int halfw = width / 2;
    int outer_r = (halfw < halfh ? halfw : halfh) - 8;
    const int inner_r = outer_r - 32;
    outer_r *= outer_r;
    const int inner_r2 = inner_r * inner_r;

    for (int y = 0; y < height; ++y) {
      const int y2 = (y - halfh) * (y - halfh);
      for (int x = 0; x < width; ++x) {
        uint32_t v;
        const int r2 = (x - halfw) * (x - halfw) + y2;
        if (r2 < inner_r2)
          v = (static_cast<uint32_t>(r2 / 32) + time / 64) * 0x0080401u;
        else if (r2 < outer_r)
          v = (static_cast<uint32_t>(y) + time / 32) * 0x0080401u;
        else
          v = (static_cast<uint32_t>(x) + time / 16) * 0x0080401u;
        v &= 0x00FFFFFFu;
        if (std::abs(x - y) > 6 && std::abs(x + y - height) > 6)
          v |= 0xFF000000u;
        pixels[y * stride + x] = v;
      }
    }
  }
};

}  // namespace wl::csd
