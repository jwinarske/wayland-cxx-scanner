// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_cairo — Cairo CSD plugin (libdecor-cairo style decorations).
//
// Following the plugin pattern from libdecor's Cairo plugin
// (https://gitlab.freedesktop.org/libdecor/libdecor/-/tree/master/src/plugins/cairo),
// this plugin uses Cairo rendering and Pango text to produce dark-themed
// decorations with colored window-control buttons and line-art symbols.
//
// The title bar, button colors, and symbol style are taken from the
// libdecor-cairo defaults.  Title text is rendered with Pango and centred
// in the available space.
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
// CairoCsdPlugin — libdecor-cairo style CSD plugin
// ══════════════════════════════════════════════════════════════════════════════

/// CSD plugin that uses Cairo and Pango to render dark-themed decorations
/// with colored window-control buttons and line-art symbols.
///
/// The aesthetic follows libdecor's Cairo plugin: a near-black title bar
/// with amber/green/orange button backgrounds and light-colored symbols.
/// When the window loses focus the title bar and symbols dim.
class CairoCsdPlugin final : public CsdPlugin {
 public:
  static constexpr int kShadowMargin = 24;
  static constexpr int kTitleHeight = 24;
  static constexpr int kButtonWidth = 32;
  static constexpr int kSymDim = 14;

  // Colors matching libdecor-cairo defaults (ARGB8888).
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

  [[nodiscard]] int BorderWidth() const noexcept override;
  [[nodiscard]] int TitleBarHeight() const noexcept override;

  void SetTitle(std::string_view title) override;
  void SetState(bool focused, bool maximized) override;

  void RenderFrame(uint32_t* buffer,
                   int surface_w,
                   int surface_h,
                   int content_w,
                   int content_h,
                   uint32_t time) override;

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
