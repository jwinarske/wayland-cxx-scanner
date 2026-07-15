// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_cairo — Cairo CSD plugin (dark-themed decorations).
//
// This plugin uses Cairo rendering and Pango text to produce dark-themed
// decorations with colored window-control buttons and line-art symbols.
//
// The title bar, button colors, and symbol style are fixed values chosen
// here, not read from any theme — which is the point of it: it draws the
// same decoration everywhere, needing nothing but Cairo and Pango.  Title
// text is rendered with Pango and centered in the available space.
//
// ── Include order
// ─────────────────────────────────────────────────────────────
//   #include <wl/csd_plugin.hpp>
//   #include <wl/csd_cairo.hpp>
//
// ── Build requirements
// ─────────────────────────────────────────────────────────
//   dependency('cairo')        →  rendering primitives
//   dependency('pangocairo')   →  title text rendering
//
// The implementation lives in a separate .cpp file
// (examples/xdg-csd/csd_cairo.cpp) because it links against Cairo/Pango.
#pragma once

#include <wl/csd_plugin.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// CairoCsdPlugin — dark-themed Cairo CSD plugin
// ══════════════════════════════════════════════════════════════════════════════

/// CSD plugin that uses Cairo and Pango to render dark-themed decorations
/// with colored window-control buttons and line-art symbols.
///
/// The aesthetic is a near-black title bar with amber/green/orange button
/// backgrounds and light-colored symbols.  When the window loses focus the
/// title bar and symbols dim.
class CairoCsdPlugin final : public CsdPlugin {
 public:
  static constexpr int kShadowMargin = 24;
  static constexpr int kBorderWidth = 1;
  static constexpr int kTitleHeight = 24;
  static constexpr int kButtonWidth = 32;
  static constexpr int kSymDim = 14;

  // Colors (ARGB8888).
  static constexpr uint32_t kColTitle = 0xFF080706;
  static constexpr uint32_t kColTitleInact = 0xFF303030;
  static constexpr uint32_t kColButtonMin = 0xFFFFBB00;
  static constexpr uint32_t kColButtonMax = 0xFF238823;
  static constexpr uint32_t kColButtonClose = 0xFFFB6542;
  static constexpr uint32_t kColSym = 0xFFF4F4EF;
  static constexpr uint32_t kColSymInact = 0xFF909090;

  CairoCsdPlugin();
  ~CairoCsdPlugin() override;

  CairoCsdPlugin(const CairoCsdPlugin&) = delete;
  CairoCsdPlugin& operator=(const CairoCsdPlugin&) = delete;
  CairoCsdPlugin(CairoCsdPlugin&&) noexcept;
  CairoCsdPlugin& operator=(CairoCsdPlugin&&) noexcept;

  // ── CsdPlugin interface ────────────────────────────────────────────────

  [[nodiscard]] Margins DecorationMargins() const override;

  void SetTitle(std::string_view title) override;
  void SetInputState(const InputState& state) override;

  void SetScale(int scale_120) override;

  void RenderDecoration(uint32_t* buffer,
                        int stride_px,
                        int surface_w,
                        int surface_h,
                        int content_w,
                        int content_h) override;

  [[nodiscard]] HitZone HitTest(int x,
                                int y,
                                int surface_w,
                                int surface_h,
                                int content_w,
                                int content_h) const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wl::csd
