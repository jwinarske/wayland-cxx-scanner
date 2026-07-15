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

#include <wl/csd_cairo.hpp>
#include <wl/csd_common.hpp>

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

// ══════════════════════════════════════════════════════════════════════════════
// Pixel helpers
// ══════════════════════════════════════════════════════════════════════════════

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
static void DrawTitleText(uint32_t* buf,
                          int buf_w,
                          int x,
                          int y,
                          int max_w,
                          int max_h,
                          const std::string& title,
                          bool active) {
  if (title.empty() || max_w <= 0 || max_h <= 0)
    return;

  // Create an ARGB32 surface for Pango rendering.
  cairo_surface_t* surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, max_w, max_h);
  cairo_t* cr = cairo_create(surface);

  // Text color: light when focused, muted when unfocused.
  const uint32_t text_col =
      active ? CairoCsdPlugin::kColSym : CairoCsdPlugin::kColSymInact;
  const auto [r, g, b, a] = common::Rgba32ToComponents(text_col);
  cairo_set_source_rgb(cr, r, g, b);

  // Layout with Pango.
  PangoLayout* layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, title.c_str(), -1);

  PangoFontDescription* font =
      pango_font_description_from_string("Sans Bold 10");
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);

  pango_layout_set_width(layout, max_w * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

  // Center vertically.
  int text_w = 0;
  int text_h = 0;
  pango_layout_get_pixel_size(layout, &text_w, &text_h);
  const int offset_y = std::max(0, (max_h - text_h) / 2);
  cairo_move_to(cr, 0, offset_y);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  cairo_destroy(cr);

  // Blit the ARGB32 surface into the ARGB8888 buffer.
  cairo_surface_flush(surface);
  const auto* src =
      reinterpret_cast<const uint32_t*>(cairo_image_surface_get_data(surface));
  const int src_stride = cairo_image_surface_get_stride(surface) / 4;

  for (int row = 0; row < max_h; ++row) {
    for (int col = 0; col < max_w; ++col) {
      const uint32_t pixel = src[row * src_stride + col];
      const uint32_t alpha = (pixel >> 24u) & 0xFFu;
      if (alpha > 0) {
        // Alpha-over compositing — Cairo ARGB32 uses premultiplied alpha,
        // so the source RGB channels are already multiplied by alpha.
        const uint32_t dst = buf[(y + row) * buf_w + (x + col)];
        const uint32_t inv_alpha = 255u - alpha;
        const uint32_t out_r =
            std::min(255u, ((pixel >> 16u) & 0xFFu) +
                               ((dst >> 16u) & 0xFFu) * inv_alpha / 255u);
        const uint32_t out_g =
            std::min(255u, ((pixel >> 8u) & 0xFFu) +
                               ((dst >> 8u) & 0xFFu) * inv_alpha / 255u);
        const uint32_t out_b =
            std::min(255u, (pixel & 0xFFu) + (dst & 0xFFu) * inv_alpha / 255u);
        buf[(y + row) * buf_w + (x + col)] =
            0xFF000000u | (out_r << 16u) | (out_g << 8u) | out_b;
      }
    }
  }

  cairo_surface_destroy(surface);
}

// ══════════════════════════════════════════════════════════════════════════════
// RenderFrame
// ══════════════════════════════════════════════════════════════════════════════

void CairoCsdPlugin::RenderDecoration(uint32_t* buffer,
                                      int surface_w,
                                      int surface_h,
                                      int content_w,
                                      int /*content_h*/) {
  constexpr int bw = kBorderWidth;

  // Borders — the four bands around the content rect (thin black outline).
  // The content area is left untouched for the application to paint.
  constexpr uint32_t kBorderColor = 0xFF000000;
  FillRect(buffer, surface_w, 0, 0, surface_w, kTitleHeight, kBorderColor);
  FillRect(buffer, surface_w, 0, surface_h - bw, surface_w, bw, kBorderColor);
  FillRect(buffer, surface_w, 0, kTitleHeight, bw,
           surface_h - kTitleHeight - bw, kBorderColor);
  FillRect(buffer, surface_w, surface_w - bw, kTitleHeight, bw,
           surface_h - kTitleHeight - bw, kBorderColor);

  // Title bar background.
  const uint32_t tb_color = impl_->state.focused ? kColTitle : kColTitleInact;
  FillRect(buffer, surface_w, bw, bw, content_w, kTitleHeight - bw, tb_color);

  // ── Button backgrounds (right-aligned in title bar) ────────────────────
  const int btn_y = bw;
  const int btn_h = kTitleHeight - bw;

  int close_x = bw + content_w - kButtonWidth;
  FillRect(buffer, surface_w, close_x, btn_y, kButtonWidth, btn_h,
           kColButtonClose);

  int max_x = close_x - kButtonWidth;
  FillRect(buffer, surface_w, max_x, btn_y, kButtonWidth, btn_h, kColButtonMax);

  int min_x = max_x - kButtonWidth;
  FillRect(buffer, surface_w, min_x, btn_y, kButtonWidth, btn_h, kColButtonMin);

  // ── Button symbols (Cairo line art) ────────────────────────────────────
  {
    cairo_surface_t* surf = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(buffer), CAIRO_FORMAT_ARGB32,
        surface_w, surface_h, surface_w * 4);
    cairo_t* cr = cairo_create(surf);

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

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
  }

  // ── Title text (Pango, centered) ────────────────────────────────────────
  constexpr int kTextPad = 8;
  const int text_x = bw + kTextPad;
  const int text_y = bw;
  const int text_max_w = content_w - 3 * kButtonWidth - 2 * kTextPad;
  const int text_max_h = kTitleHeight - bw;
  if (text_max_w > 0) {
    DrawTitleText(buffer, surface_w, text_x, text_y, text_max_w, text_max_h,
                  impl_->title, impl_->state.focused);
  }
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
