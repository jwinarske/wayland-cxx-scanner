// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_cairo.cpp — Cairo CSD plugin implementation.
//
// Uses Cairo and Pango to render libdecor-cairo style window decorations:
// a near-black title bar with colored button backgrounds, line-art
// symbols (close ×, maximize □/⧉, minimize ─), and centered Pango title
// text.
//
// Following the plugin pattern from libdecor's Cairo plugin:
// https://gitlab.freedesktop.org/libdecor/libdecor/-/tree/master/src/plugins/cairo

// clang-tidy: suppress diagnostics for C-API boundary code.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,
//             cppcoreguidelines-pro-bounds-pointer-arithmetic,
//             cppcoreguidelines-avoid-non-const-global-variables)

#include "csd_cairo.hpp"
#include <wl/csd_common.hpp>
#include <wl/scale_policy.hpp>

#include <algorithm>
#include <string>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// CairoCsdPlugin::Impl — pimpl holding rendering state
// ══════════════════════════════════════════════════════════════════════════════

struct CairoCsdPlugin::Impl {
  std::string title;
  InputState state;
  int scale_120 = ScalePolicy::kUnityScale120;
};

// ══════════════════════════════════════════════════════════════════════════════
// CairoCsdPlugin — public interface implementation
// ══════════════════════════════════════════════════════════════════════════════

CairoCsdPlugin::CairoCsdPlugin() : impl_(std::make_unique<Impl>()) {}

CairoCsdPlugin::~CairoCsdPlugin() = default;

CairoCsdPlugin::CairoCsdPlugin(CairoCsdPlugin&&) noexcept = default;
CairoCsdPlugin& CairoCsdPlugin::operator=(CairoCsdPlugin&&) noexcept = default;

Margins CairoCsdPlugin::DecorationMargins() const {
  return {kBorderWidth, kBorderWidth, kTitleHeight, kBorderWidth};
}

void CairoCsdPlugin::SetTitle(std::string_view title) {
  impl_->title = title;
}

void CairoCsdPlugin::SetInputState(const InputState& state) {
  impl_->state = state;
}

void CairoCsdPlugin::SetScale(int scale_120) {
  impl_->scale_120 = scale_120;
}

// ══════════════════════════════════════════════════════════════════════════════
// Button symbol drawing (Cairo line art)
// ══════════════════════════════════════════════════════════════════════════════

/// Draw a close (×) symbol.
static void DrawCloseSymbol(cairo_t* cr,
                            double x,
                            double y,
                            int sym_dim,
                            uint32_t color) {
  common::CairoSetRgba32(cr, color);
  cairo_set_line_width(cr, 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  const double d = static_cast<double>(sym_dim);
  constexpr double pad = 2.0;
  cairo_move_to(cr, x + pad, y + pad);
  cairo_line_to(cr, x + d - pad, y + d - pad);
  cairo_move_to(cr, x + d - pad, y + pad);
  cairo_line_to(cr, x + pad, y + d - pad);
  cairo_stroke(cr);
}

/// Draw a maximize (□) or restore (⧉) symbol.
static void DrawMaximizeSymbol(cairo_t* cr,
                               double x,
                               double y,
                               int sym_dim,
                               uint32_t color,
                               bool is_maximized) {
  common::CairoSetRgba32(cr, color);
  cairo_set_line_width(cr, 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);

  const double d = static_cast<double>(sym_dim);

  if (!is_maximized) {
    // Single rectangle outline.
    cairo_rectangle(cr, x + 2, y + 2, d - 4, d - 4);
    cairo_stroke(cr);
  } else {
    // Two overlapping rectangles (restore symbol).
    // Front rectangle (lower-left).
    cairo_rectangle(cr, x + 1, y + 4, d - 5, d - 5);
    cairo_stroke(cr);
    // Back rectangle (upper-right, partially hidden).
    cairo_move_to(cr, x + 4, y + 4);
    cairo_line_to(cr, x + 4, y + 1);
    cairo_line_to(cr, x + d - 1, y + 1);
    cairo_line_to(cr, x + d - 1, y + d - 4);
    cairo_line_to(cr, x + d - 5 + 1, y + d - 4);
    cairo_stroke(cr);
  }
}

/// Draw a minimize (─) symbol.
static void DrawMinimizeSymbol(cairo_t* cr,
                               double x,
                               double y,
                               int sym_dim,
                               uint32_t color) {
  common::CairoSetRgba32(cr, color);
  cairo_set_line_width(cr, 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  const double d = static_cast<double>(sym_dim);
  constexpr double pad = 2.0;
  const double cy = y + d - pad - 1.0;
  cairo_move_to(cr, x + pad, cy);
  cairo_line_to(cr, x + d - pad, cy);
  cairo_stroke(cr);
}

// ══════════════════════════════════════════════════════════════════════════════
// Title text rendering (Pango + Cairo → ARGB8888)
// ══════════════════════════════════════════════════════════════════════════════

/// Render title text using Pango into the ARGB8888 buffer with alpha
/// compositing.  The text is centered horizontally and vertically within
/// the given rectangle.
/// Render the title with Pango straight onto @p cr, centred in the given
/// rectangle.
///
/// Drawn through the caller's context rather than blitted through a scratch
/// buffer: the context carries the device scale, so the glyphs are rasterized
/// at the panel's resolution instead of at logical size and stretched.
static void DrawTitleText(cairo_t* cr,
                          int x,
                          int y,
                          int max_w,
                          int max_h,
                          const std::string& title,
                          bool active) {
  if (title.empty() || max_w <= 0 || max_h <= 0)
    return;

  cairo_save(cr);
  cairo_rectangle(cr, x, y, max_w, max_h);
  cairo_clip(cr);

  // Text color: light when focused, muted when unfocused.
  const uint32_t text_col =
      active ? CairoCsdPlugin::kColSym : CairoCsdPlugin::kColSymInact;
  const auto [r, g, b, a] = common::Rgba32ToComponents(text_col);
  cairo_set_source_rgb(cr, r, g, b);

  PangoLayout* layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, title.c_str(), -1);

  PangoFontDescription* font =
      pango_font_description_from_string("Sans Bold 10");
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);

  pango_layout_set_width(layout, max_w * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

  int text_w = 0;
  int text_h = 0;
  pango_layout_get_pixel_size(layout, &text_w, &text_h);
  cairo_move_to(cr, x, y + std::max(0, (max_h - text_h) / 2));
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  cairo_restore(cr);
}

// ══════════════════════════════════════════════════════════════════════════════
// RenderFrame
// ══════════════════════════════════════════════════════════════════════════════

void CairoCsdPlugin::RenderDecoration(uint32_t* buffer,
                                      int stride_px,
                                      int surface_w,
                                      int surface_h,
                                      int content_w,
                                      int /*content_h*/) {
  // Drawn entirely through Cairo in logical units, with the device scale
  // turning them into the panel's physical pixels. That keeps the line art and
  // the title text at the panel's real resolution instead of drawn small and
  // stretched, and it means none of the geometry below has to know the scale.
  const int phys_w = ScalePolicy::ScaledDim(surface_w, impl_->scale_120);
  const int phys_h = ScalePolicy::ScaledDim(surface_h, impl_->scale_120);
  cairo_surface_t* surf = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(buffer), CAIRO_FORMAT_ARGB32, phys_w,
      phys_h, stride_px * 4);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return;
  }
  const double scale = static_cast<double>(impl_->scale_120) /
                       static_cast<double>(ScalePolicy::kUnityScale120);
  cairo_surface_set_device_scale(surf, scale, scale);

  cairo_t* cr = cairo_create(surf);
  constexpr int bw = kBorderWidth;

  auto fill = [&](double x, double y, double w, double h, uint32_t color) {
    common::CairoSetRgba32(cr, color);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
  };

  // Borders — the four bands around the content rect (thin black outline).
  // The content area is left untouched for the application to paint.
  constexpr uint32_t kBorderColor = 0xFF000000;
  fill(0, 0, surface_w, kTitleHeight, kBorderColor);
  fill(0, surface_h - bw, surface_w, bw, kBorderColor);
  fill(0, kTitleHeight, bw, surface_h - kTitleHeight - bw, kBorderColor);
  fill(surface_w - bw, kTitleHeight, bw, surface_h - kTitleHeight - bw,
       kBorderColor);

  // Title bar background.
  const uint32_t tb_color = impl_->state.focused ? kColTitle : kColTitleInact;
  fill(bw, bw, content_w, kTitleHeight - bw, tb_color);

  // ── Button backgrounds (right-aligned in title bar) ────────────────────
  const int btn_y = bw;
  const int btn_h = kTitleHeight - bw;

  const int close_x = bw + content_w - kButtonWidth;
  fill(close_x, btn_y, kButtonWidth, btn_h, kColButtonClose);
  const int max_x = close_x - kButtonWidth;
  fill(max_x, btn_y, kButtonWidth, btn_h, kColButtonMax);
  const int min_x = max_x - kButtonWidth;
  fill(min_x, btn_y, kButtonWidth, btn_h, kColButtonMin);

  // ── Button symbols (Cairo line art) ────────────────────────────────────
  const uint32_t sym_col = impl_->state.focused ? kColSym : kColSymInact;
  const int sym_ox = (kButtonWidth - kSymDim) / 2;
  const int sym_oy = (btn_h - kSymDim) / 2;

  DrawCloseSymbol(cr, static_cast<double>(close_x + sym_ox),
                  static_cast<double>(btn_y + sym_oy), kSymDim, sym_col);
  DrawMaximizeSymbol(cr, static_cast<double>(max_x + sym_ox),
                     static_cast<double>(btn_y + sym_oy), kSymDim, sym_col,
                     impl_->state.maximized);
  DrawMinimizeSymbol(cr, static_cast<double>(min_x + sym_ox),
                     static_cast<double>(btn_y + sym_oy), kSymDim, sym_col);

  // ── Title text (Pango, centered) ────────────────────────────────────────
  constexpr int kTextPad = 8;
  const int text_max_w = content_w - 3 * kButtonWidth - 2 * kTextPad;
  if (text_max_w > 0) {
    DrawTitleText(cr, bw + kTextPad, bw, text_max_w, kTitleHeight - bw,
                  impl_->title, impl_->state.focused);
  }

  cairo_destroy(cr);
  cairo_surface_flush(surf);
  cairo_surface_destroy(surf);
}

// ══════════════════════════════════════════════════════════════════════════════
// HitTest
// ══════════════════════════════════════════════════════════════════════════════

HitZone CairoCsdPlugin::HitTest(int x,
                                int y,
                                int surface_w,
                                int surface_h,
                                int content_w,
                                int /*content_h*/) const noexcept {
  constexpr int bw = 1;

  if (x < 0 || y < 0 || x >= surface_w || y >= surface_h)
    return HitZone::None;

  // Corner resize zones (border × border squares at corners).
  if (x < bw && y < bw)
    return HitZone::ResizeTopLeft;
  if (x >= surface_w - bw && y < bw)
    return HitZone::ResizeTopRight;
  if (x < bw && y >= surface_h - bw)
    return HitZone::ResizeBottomLeft;
  if (x >= surface_w - bw && y >= surface_h - bw)
    return HitZone::ResizeBottomRight;

  // Edge resize zones.
  if (y < bw)
    return HitZone::ResizeTop;
  if (y >= surface_h - bw)
    return HitZone::ResizeBottom;
  if (x < bw)
    return HitZone::ResizeLeft;
  if (x >= surface_w - bw)
    return HitZone::ResizeRight;

  // Title bar region.
  if (y < kTitleHeight) {
    // Check buttons (right-aligned, full title-bar height).
    int btn_x = bw + content_w - kButtonWidth;
    if (x >= btn_x && x < btn_x + kButtonWidth && y >= bw)
      return HitZone::CloseButton;

    btn_x -= kButtonWidth;
    if (x >= btn_x && x < btn_x + kButtonWidth && y >= bw)
      return HitZone::MaximizeButton;

    btn_x -= kButtonWidth;
    if (x >= btn_x && x < btn_x + kButtonWidth && y >= bw)
      return HitZone::MinimizeButton;

    return HitZone::TitleBar;
  }

  return HitZone::Content;
}

}  // namespace wl::csd

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-avoid-non-const-global-variables)
