// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_gtk — GTK-themed CSD plugin (optional; requires GTK 3).
//
// Renders decorations that match the user's GTK theme by building a real
// header bar widget offscreen and asking GTK to lay it out, draw it and
// measure it.  Nothing about the decoration is approximated locally: the title
// bar height, the font, which window buttons exist and in what order, their
// icons, and the hover/pressed/backdrop styling are all GTK's answers for the
// active theme.
//
// Notably the button set is never parsed out of a setting.  GTK builds the
// header bar from gtk-decoration-layout, so a desktop configured to show only
// a close button gets exactly that, with no code here aware of the option.
//
// When GTK is unavailable at build time the Cairo or fallback plugin is used
// instead; that selection happens in the build (the csd / csd_gtk options).
// When GTK is present but unusable at *run* time, TryCreate() returns null so
// the caller can degrade rather than abort.
//
// ── Include order
// ─────────────────────────────────────────────────────────────
//   #include <wl/csd_plugin.hpp>
//   #include <wl/csd_gtk.hpp>
//
// ── Build requirements
// ─────────────────────────────────────────────────────────
//   dependency('gtk+-3.0')    →  provides GtkHeaderBar, Cairo, Pango
//
// The implementation lives in separate .cpp files under examples/xdg-csd/
// because it links against GTK.
#pragma once

#include <wl/csd_plugin.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// GtkCsdPlugin — GTK-themed CSD plugin
// ══════════════════════════════════════════════════════════════════════════════

/// CSD plugin that renders its decorations through GTK's own widget theming.
///
/// Construct with TryCreate(): GTK may be linked but unusable at run time (no
/// display, no theme), and that must degrade to another plugin rather than
/// abort the process.
class GtkCsdPlugin final : public CsdPlugin {
 public:
  /// How far from a corner a grab counts as a diagonal resize.
  ///
  /// The one decoration measurement that is ours: the shadow's size, offset,
  /// blur and color all come from the theme, but how big a diagonal resize
  /// target should be is an input question, not a styling one.
  static constexpr int kCornerSize = 32;

  /// Build the plugin and bring GTK up.
  ///
  /// @returns null when GTK cannot be initialized, leaving the caller to fall
  ///          back to another plugin.
  [[nodiscard]] static std::unique_ptr<GtkCsdPlugin> TryCreate();

  ~GtkCsdPlugin() override;

  GtkCsdPlugin(const GtkCsdPlugin&) = delete;
  GtkCsdPlugin& operator=(const GtkCsdPlugin&) = delete;
  GtkCsdPlugin(GtkCsdPlugin&&) noexcept;
  GtkCsdPlugin& operator=(GtkCsdPlugin&&) noexcept;

  // ── CsdPlugin interface ────────────────────────────────────────────────

  [[nodiscard]] Margins DecorationMargins() const override;
  [[nodiscard]] Margins ShadowMargins() const override;

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

  [[nodiscard]] const char* CursorThemeName() const override;
  [[nodiscard]] int CursorSize() const override;
  [[nodiscard]] int DoubleClickTimeMs() const override;
  [[nodiscard]] int DragThreshold() const override;

  void Dispatch() override;

 private:
  GtkCsdPlugin();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wl::csd
