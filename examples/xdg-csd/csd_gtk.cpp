// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_gtk.cpp — themed CSD plugin.
//
// This file deliberately does not include gtk.h.  Everything needing a widget
// tree lives behind <csd_gtk_backend.hpp>; what remains here is the part that
// is not the toolkit's business: the border fills, the surface geometry, and
// compositing the header the backend drew into the wl_shm buffer.
//
// The header bar is not approximated.  Its height, font, button set, icons and
// styling are whatever the backend's toolkit produces for the active theme.

// clang-tidy: suppress diagnostics for C-API boundary code.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,
//             cppcoreguidelines-pro-bounds-pointer-arithmetic)

#include <wl/csd_gtk.hpp>

#include "csd_gtk_backend.hpp"

#include <algorithm>
#include <memory>

#include <cairo/cairo.h>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// GtkCsdPlugin::Impl
// ══════════════════════════════════════════════════════════════════════════════

struct GtkCsdPlugin::Impl {
  std::unique_ptr<detail::GtkThemeBackend> backend;
  InputState state;

  // Cached header height, refreshed per redraw.  DecorationMargins() is
  // const, and the geometry must not silently disagree with what was drawn.
  int header_height = 0;
};

// ══════════════════════════════════════════════════════════════════════════════
// Construction
// ══════════════════════════════════════════════════════════════════════════════

GtkCsdPlugin::GtkCsdPlugin() : impl_(std::make_unique<Impl>()) {}

GtkCsdPlugin::~GtkCsdPlugin() = default;

GtkCsdPlugin::GtkCsdPlugin(GtkCsdPlugin&&) noexcept = default;
GtkCsdPlugin& GtkCsdPlugin::operator=(GtkCsdPlugin&&) noexcept = default;

std::unique_ptr<GtkCsdPlugin> GtkCsdPlugin::TryCreate() {
  auto plugin = std::unique_ptr<GtkCsdPlugin>(new GtkCsdPlugin());
  plugin->impl_->backend = detail::MakeGtk3Backend();
  if (!plugin->impl_->backend->Init())
    return nullptr;  // No usable GTK; the caller picks another plugin.
  plugin->impl_->header_height = plugin->impl_->backend->HeaderHeight();
  return plugin;
}

// ══════════════════════════════════════════════════════════════════════════════
// CsdPlugin interface
// ══════════════════════════════════════════════════════════════════════════════

Margins GtkCsdPlugin::DecorationMargins() const {
  // Height comes from the theme, so it is re-measured rather than fixed.  The
  // side and bottom borders stay flat for now; they become the shadow once the
  // decoration grows one.
  if (impl_->backend)
    impl_->header_height = impl_->backend->HeaderHeight();
  return {kBorderWidth, kBorderWidth, std::max(impl_->header_height, 1),
          kBorderWidth};
}

void GtkCsdPlugin::SetTitle(std::string_view title) {
  if (impl_->backend)
    impl_->backend->SetTitle(title);
}

void GtkCsdPlugin::SetInputState(const InputState& state) {
  impl_->state = state;
  // The header spans the full surface width and starts at y=0, so surface-local
  // pointer coordinates are already header-local.
  if (impl_->backend)
    impl_->backend->SetInputState(state);
}

void GtkCsdPlugin::Dispatch() {
  if (impl_->backend)
    impl_->backend->Dispatch();
}

// ── Rendering ───────────────────────────────────────────────────────────────

namespace {

void FillRect(uint32_t* buf,
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

}  // namespace

void GtkCsdPlugin::RenderDecoration(uint32_t* buffer,
                                    int surface_w,
                                    int surface_h,
                                    int content_w,
                                    int /*content_h*/) {
  const Margins m = DecorationMargins();

  // Borders — the bands left, right and below the content rect.  The content
  // area itself is the application's to paint.
  FillRect(buffer, surface_w, 0, surface_h - m.bottom, surface_w, m.bottom,
           kBorderColor);
  FillRect(buffer, surface_w, 0, m.top, m.left, surface_h - m.top - m.bottom,
           kBorderColor);
  FillRect(buffer, surface_w, surface_w - m.right, m.top, m.right,
           surface_h - m.top - m.bottom, kBorderColor);

  if (!impl_->backend) {
    FillRect(buffer, surface_w, 0, 0, surface_w, m.top, kBorderColor);
    return;
  }

  // Hand the backend an ARGB32 surface over the title-bar band and let it draw
  // the header there.  cairo_image_surface_create_for_data writes straight into
  // the wl_shm buffer, so there is no intermediate copy -- the band is the full
  // surface width at y=0, so it starts at a row boundary.
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(buffer), CAIRO_FORMAT_ARGB32, surface_w,
      m.top, surface_w * 4);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    FillRect(buffer, surface_w, 0, 0, surface_w, m.top, kBorderColor);
    return;
  }

  // Clear to transparent first.  The theme rounds the header's top corners and
  // leaves those pixels alone, and the buffer is recycled between frames, so
  // whatever was there before would otherwise show through the curve.
  FillRect(buffer, surface_w, 0, 0, surface_w, m.top, 0x00000000u);

  // The header spans the full surface width rather than being inset by the
  // border, so the corners the theme rounds are the window's own corners.  A
  // flat border strip beside them would frame the rounding in a square edge and
  // defeat the point.
  impl_->backend->DrawHeader(surface, surface_w);
  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
}

// ── Hit testing ─────────────────────────────────────────────────────────────

HitZone GtkCsdPlugin::HitTest(int x,
                              int y,
                              int surface_w,
                              int surface_h,
                              int /*content_w*/,
                              int /*content_h*/) const noexcept {
  if (x < 0 || y < 0 || x >= surface_w || y >= surface_h)
    return HitZone::None;

  const int top = std::max(impl_->header_height, 1);

  // Resize zones: corners first, then edges.
  if (x < kBorderWidth && y < kBorderWidth)
    return HitZone::ResizeTopLeft;
  if (x >= surface_w - kBorderWidth && y < kBorderWidth)
    return HitZone::ResizeTopRight;
  if (x < kBorderWidth && y >= surface_h - kBorderWidth)
    return HitZone::ResizeBottomLeft;
  if (x >= surface_w - kBorderWidth && y >= surface_h - kBorderWidth)
    return HitZone::ResizeBottomRight;
  if (y < kBorderWidth)
    return HitZone::ResizeTop;
  if (y >= surface_h - kBorderWidth)
    return HitZone::ResizeBottom;
  if (x < kBorderWidth)
    return HitZone::ResizeLeft;
  if (x >= surface_w - kBorderWidth)
    return HitZone::ResizeRight;

  // Title bar: ask the backend, so the answer comes from the same widget
  // geometry that was drawn rather than from a second guess at where the
  // buttons are.
  if (y < top) {
    if (!impl_->backend)
      return HitZone::TitleBar;
    return impl_->backend->HitTestHeader(x, y, surface_w);
  }

  return HitZone::Content;
}

}  // namespace wl::csd

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic)
