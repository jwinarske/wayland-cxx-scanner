// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_gtk — GTK-themed CSD plugin (optional; requires GTK 3).
//
// Following the plugin pattern from libdecor's GTK plugin
// (https://gitlab.freedesktop.org/libdecor/libdecor/-/tree/master/src/plugins/gtk),
// this plugin uses GTK 3's CSS theming engine and Cairo rendering to produce
// decorations that match the user's GTK theme.  Colours for the title bar,
// borders, and window-control buttons are extracted from the GTK style
// context, and the title text is drawn with Pango.
//
// When GTK 3 is not available at build time the fallback plugin
// (<wl/csd_fallback.hpp>) is used instead — the selection happens in the
// meson build and through the factory in <wl/csd_plugin.hpp>.
//
// ── Include order
// ─────────────────────────────────────────────────────────────
//   #include <wl/csd_plugin.hpp>
//   #include <wl/csd_gtk.hpp>
//
// ── Build requirements
// ─────────────────────────────────────────────────────────
//   dependency('gtk+-3.0')    →  provides GtkStyleContext, Cairo, Pango
//
// The implementation lives in a separate .cpp file
// (examples/xdg-csd/csd_gtk.cpp) because it links against GTK3.
#pragma once

#include <wl/csd_plugin.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// GtkCsdPlugin — GTK-themed CSD plugin
// ══════════════════════════════════════════════════════════════════════════════

/// CSD plugin that uses GTK 3's style context and Cairo to render
/// decorations matching the user's GTK theme.
///
/// The title bar colour, border colour, button colours and title text are
/// all derived from the active GTK theme.  When the window loses focus the
/// decoration dims to the theme's backdrop style.
class GtkCsdPlugin final : public CsdPlugin {
 public:
  static constexpr int kBorderWidth = 4;
  static constexpr int kTitleBarHeight = 36;
  static constexpr int kButtonSize = 20;
  static constexpr int kButtonPadding = 6;

  GtkCsdPlugin();
  ~GtkCsdPlugin() override;

  GtkCsdPlugin(const GtkCsdPlugin&) = delete;
  GtkCsdPlugin& operator=(const GtkCsdPlugin&) = delete;
  GtkCsdPlugin(GtkCsdPlugin&&) noexcept;
  GtkCsdPlugin& operator=(GtkCsdPlugin&&) noexcept;

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
