// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_gtk_backend — internal seam between the GTK CSD plugin and the GTK
// version it renders through.
//
// The plugin (csd_gtk.cpp) owns the parts that are not GTK's business: the
// border fills, the surface geometry, and the blit into the wl_shm buffer.
// Everything that needs a GTK widget tree — measuring the header bar, drawing
// it, and hit-testing it — lives behind this interface, in a translation unit
// that is the only one including gtk.h.
//
// The split exists because the two GTK majors cannot share an implementation:
// GTK 4 removed the offscreen widget-draw path the GTK 3 backend renders
// through, so it needs a backend of its own rather than #ifdefs threaded
// through the plugin.  Only the GTK 3 backend exists today.
//
// Cairo is the currency deliberately: GTK 3 draws to a cairo_t natively, and a
// GTK 4 backend could render to a texture and hand back a surface without this
// interface changing.
#pragma once

#include <wl/csd_plugin.hpp>

#include <cairo/cairo.h>

#include <memory>
#include <string_view>

namespace wl::csd::detail {

/// Renders and measures a window header bar using a toolkit's own theming.
class GtkThemeBackend {
 public:
  virtual ~GtkThemeBackend() = default;

  GtkThemeBackend() = default;
  GtkThemeBackend(const GtkThemeBackend&) = delete;
  GtkThemeBackend& operator=(const GtkThemeBackend&) = delete;
  GtkThemeBackend(GtkThemeBackend&&) = delete;
  GtkThemeBackend& operator=(GtkThemeBackend&&) = delete;

  /// Bring the toolkit up and build the widget tree.
  ///
  /// @returns false when the toolkit is unavailable (no display, no theme).
  ///          The caller degrades to another plugin rather than aborting.
  [[nodiscard]] virtual bool Init() = 0;

  /// Set the title the header bar displays.
  virtual void SetTitle(std::string_view title) = 0;

  /// Update pointer/window state so the header renders hovered, pressed,
  /// backdrop and maximized states the way the theme defines them.
  virtual void SetInputState(const InputState& state) = 0;

  /// The header bar's height, as the theme wants it.  Measured, not fixed:
  /// the answer changes with the theme, so callers must not cache it.
  [[nodiscard]] virtual int HeaderHeight() = 0;

  /// Lay the header out to @p width and draw it into the top-left of
  /// @p surface, which must be an ARGB32 image surface.
  virtual void DrawHeader(cairo_surface_t* surface, int width) = 0;

  /// Which zone of the header is at (@p x, @p y), in header-local pixels,
  /// when the header is laid out to @p width.
  ///
  /// Answered from the real widget geometry, so it always agrees with what
  /// DrawHeader drew.  @p width is taken rather than assumed so that this is
  /// correct even before the first DrawHeader: hit-testing must not depend on
  /// having been rendered first.
  [[nodiscard]] virtual HitZone HitTestHeader(int x, int y, int width) = 0;

  /// Drain the toolkit's event source without blocking, so it observes
  /// settings and theme changes.
  virtual void Dispatch() = 0;
};

/// Construct the GTK 3 backend.  Never null; call Init() to find out whether
/// GTK is actually usable.
[[nodiscard]] std::unique_ptr<GtkThemeBackend> MakeGtk3Backend();

}  // namespace wl::csd::detail
