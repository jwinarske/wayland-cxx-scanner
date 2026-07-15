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

  void RenderDecoration(uint32_t* buffer,
                        int surface_w,
                        int surface_h,
                        int content_w,
                        int /*content_h*/) override {
    // Borders — the four bands around the content rect.  The content area is
    // left untouched for the application to paint.
    FillRect(buffer, surface_w, 0, 0, surface_w, kTitleBarHeight, kColorBorder);
    FillRect(buffer, surface_w, 0, surface_h - kBorderWidth, surface_w,
             kBorderWidth, kColorBorder);
    FillRect(buffer, surface_w, 0, kTitleBarHeight, kBorderWidth,
             surface_h - kTitleBarHeight - kBorderWidth, kColorBorder);
    FillRect(buffer, surface_w, surface_w - kBorderWidth, kTitleBarHeight,
             kBorderWidth, surface_h - kTitleBarHeight - kBorderWidth,
             kColorBorder);

    // Title bar.
    const uint32_t tb_color =
        state_.focused ? kColorTitleBar : kColorTitleBarUnfocused;
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
