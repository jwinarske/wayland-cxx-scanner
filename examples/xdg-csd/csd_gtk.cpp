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

#include <wl/scale_policy.hpp>

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

  // Cached header height and shadow margin, refreshed per redraw.  Both are
  // the theme's answers and change with it; DecorationMargins() is const, and
  // the geometry must not silently disagree with what was drawn.
  int header_height = 0;
  Margins shadow;
  int scale_120 = ScalePolicy::kUnityScale120;
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
  // Both numbers are the theme's, so both are re-measured rather than fixed.
  // The shadow surrounds the whole window — title bar included — so it adds its
  // margin to every edge, and to the top on top of the title bar.
  if (impl_->backend) {
    impl_->header_height = impl_->backend->HeaderHeight();
    impl_->shadow = impl_->backend->ShadowMargin();
  }
  const Margins& sm = impl_->shadow;
  return {sm.left, sm.right, sm.top + std::max(impl_->header_height, 1),
          sm.bottom};
}

Margins GtkCsdPlugin::ShadowMargins() const {
  // The shadow is drawn outside the window and is grabbable, but it is not the
  // window: it must stay out of the window geometry, or the compositor aligns
  // and constrains the window as though the shadow were part of it.
  //
  // Zero when the theme drops the shadow, which it does for a maximized window
  // — and then the window geometry is the whole surface, with nothing to
  // subtract.
  if (impl_->backend)
    impl_->shadow = impl_->backend->ShadowMargin();
  return impl_->shadow;
}

void GtkCsdPlugin::SetTitle(std::string_view title) {
  if (impl_->backend)
    impl_->backend->SetTitle(title);
}

void GtkCsdPlugin::SetInputState(const InputState& state) {
  impl_->state = state;
  if (!impl_->backend)
    return;
  // The backend needs the raw state first: the shadow margin it reports
  // depends on whether the window is maximized.
  impl_->backend->SetInputState(state);
  impl_->shadow = impl_->backend->ShadowMargin();

  // The header sits inside the shadow margin, so the pointer has to be moved
  // into header-local coordinates before the theme is asked about it.
  InputState local = state;
  if (state.pointer_x >= 0 && state.pointer_y >= 0) {
    local.pointer_x = state.pointer_x - impl_->shadow.left;
    local.pointer_y = state.pointer_y - impl_->shadow.top;
  }
  impl_->backend->SetInputState(local);
}

void GtkCsdPlugin::SetScale(int scale_120) {
  impl_->scale_120 = scale_120;
}

int GtkCsdPlugin::DoubleClickTimeMs() const {
  return impl_->backend ? impl_->backend->DoubleClickTimeMs() : 400;
}

int GtkCsdPlugin::DragThreshold() const {
  return impl_->backend ? impl_->backend->DragThreshold() : 8;
}

void GtkCsdPlugin::Dispatch() {
  if (impl_->backend)
    impl_->backend->Dispatch();
}

// ── Rendering ───────────────────────────────────────────────────────────────

void GtkCsdPlugin::RenderDecoration(uint32_t* buffer,
                                    int stride_px,
                                    int surface_w,
                                    int surface_h,
                                    int content_w,
                                    int content_h) {
  const Margins m = DecorationMargins();
  const Margins& sm = impl_->shadow;

  // The window — title bar plus content — sits inside the shadow margin.
  const int win_x = sm.left;
  const int win_y = sm.top;
  const int win_w = content_w;
  const int win_h = (m.top - sm.top) + content_h;

  // One Cairo pass over the whole surface: the shadow is translucent and the
  // theme rounds the title bar's corners, so this composites rather than fills.
  // Writing straight into the wl_shm buffer keeps it to a single pass.
  const int phys_w = ScalePolicy::ScaledDim(surface_w, impl_->scale_120);
  const int phys_h = ScalePolicy::ScaledDim(surface_h, impl_->scale_120);
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(buffer), CAIRO_FORMAT_ARGB32, phys_w,
      phys_h, stride_px * 4);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return;
  }

  // Everything below is drawn in logical units; the device scale is what turns
  // them into the panel's physical pixels. GTK renders through it, so the text
  // and the symbolic icons come out at the panel's real resolution rather than
  // drawn small and stretched — and the backend reads this same device scale
  // back off the surface to ask the icon theme for the right size.
  const double s = static_cast<double>(impl_->scale_120) /
                   static_cast<double>(ScalePolicy::kUnityScale120);
  cairo_surface_set_device_scale(surface, s, s);

  // Clear the decoration to transparent, leaving the content rect alone: it is
  // the application's, painted after this returns. The buffer is recycled, so
  // anything left behind shows through the shadow and the rounded corners.
  cairo_t* cr = cairo_create(surface);
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_rectangle(cr, 0, 0, surface_w, surface_h);
  cairo_rectangle(cr, m.left, m.top, content_w, content_h);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);  // everything but content
  cairo_fill(cr);
  cairo_restore(cr);
  cairo_destroy(cr);

  if (impl_->backend == nullptr)
    return;

  // The theme draws the decoration — drop shadow, corner radius, hairline —
  // for the whole window rect. Nothing here decides what a shadow looks like,
  // so it follows focus, and vanishes when maximized, on its own.
  impl_->backend->DrawDecoration(surface, win_x, win_y, win_w, win_h);

  // Then the header, over the decoration's rounded body, in its own origin.
  cairo_surface_t* header = cairo_surface_create_for_rectangle(
      surface, win_x, win_y, win_w, m.top - sm.top);
  if (cairo_surface_status(header) == CAIRO_STATUS_SUCCESS)
    impl_->backend->DrawHeader(header, win_w);
  cairo_surface_destroy(header);

  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
}

// ── Hit testing ─────────────────────────────────────────────────────────────

HitZone GtkCsdPlugin::HitTest(int x,
                              int y,
                              int surface_w,
                              int surface_h,
                              int content_w,
                              int /*content_h*/) const noexcept {
  if (x < 0 || y < 0 || x >= surface_w || y >= surface_h)
    return HitZone::None;

  const int header_h = std::max(impl_->header_height, 1);
  const Margins& sm = impl_->shadow;

  // The shadow margin is the resize grab area: invisible, but the only place a
  // pointer can reach an edge, since the window itself is the title bar and the
  // application's content. A maximized window has no shadow and so no grab
  // margin — which is right, there is nothing to resize it to.
  const bool left = x < sm.left;
  const bool right = x >= surface_w - sm.right;
  const bool top = y < sm.top;
  const bool bottom = y >= surface_h - sm.bottom;

  if (left || right || top || bottom) {
    // Corners win over edges, and reach further along each edge than the
    // margin is thick — a 24px diagonal target is hard to hit otherwise.
    const bool near_l = x < kCornerSize;
    const bool near_r = x >= surface_w - kCornerSize;
    const bool near_t = y < kCornerSize;
    const bool near_b = y >= surface_h - kCornerSize;

    if (near_t && near_l)
      return HitZone::ResizeTopLeft;
    if (near_t && near_r)
      return HitZone::ResizeTopRight;
    if (near_b && near_l)
      return HitZone::ResizeBottomLeft;
    if (near_b && near_r)
      return HitZone::ResizeBottomRight;
    if (top)
      return HitZone::ResizeTop;
    if (bottom)
      return HitZone::ResizeBottom;
    if (left)
      return HitZone::ResizeLeft;
    return HitZone::ResizeRight;
  }

  // Title bar: ask the backend, so the answer comes from the same widget
  // geometry that was drawn rather than from a second guess at where the
  // buttons are.
  if (y < sm.top + header_h) {
    if (!impl_->backend)
      return HitZone::TitleBar;
    return impl_->backend->HitTestHeader(x - sm.left, y - sm.top, content_w);
  }

  return HitZone::Content;
}

}  // namespace wl::csd

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic)
