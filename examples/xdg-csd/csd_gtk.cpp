// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_gtk.cpp — GTK-themed CSD plugin implementation.
//
// Uses GTK 3's CSS theming engine, Cairo, and Pango to render window
// decorations (title bar, window buttons, resize borders) that match
// the user's active GTK theme.
//
// Following the plugin pattern from libdecor's GTK plugin:
// https://gitlab.freedesktop.org/libdecor/libdecor/-/tree/master/src/plugins/gtk

// clang-tidy: suppress diagnostics for C-API boundary code.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,
//             cppcoreguidelines-pro-bounds-pointer-arithmetic,
//             cppcoreguidelines-avoid-non-const-global-variables)

#include <wl/csd_gtk.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

namespace wl::csd {

// ══════════════════════════════════════════════════════════════════════════════
// Theme colour extraction helpers
// ══════════════════════════════════════════════════════════════════════════════

/// ARGB8888 from GdkRGBA.
static uint32_t GdkColorToArgb(const GdkRGBA& c) noexcept {
  const auto a = static_cast<uint32_t>(c.alpha * 255.0) & 0xFFu;
  const auto r = static_cast<uint32_t>(c.red * 255.0) & 0xFFu;
  const auto g = static_cast<uint32_t>(c.green * 255.0) & 0xFFu;
  const auto b = static_cast<uint32_t>(c.blue * 255.0) & 0xFFu;
  return (a << 24u) | (r << 16u) | (g << 8u) | b;
}

// ══════════════════════════════════════════════════════════════════════════════
// GtkCsdPlugin::Impl — pimpl holding GTK/Cairo state
// ══════════════════════════════════════════════════════════════════════════════

struct GtkCsdPlugin::Impl {
  std::string title;
  bool focused = true;
  bool maximized = false;

  // Cached theme colours (refreshed on state change).
  uint32_t title_bar_color = 0xFF3C3C3C;
  uint32_t title_bar_unfocused_color = 0xFF505050;
  uint32_t border_color = 0xFF505050;
  uint32_t close_btn_color = 0xFFE04040;
  uint32_t max_btn_color = 0xFF40A040;
  uint32_t min_btn_color = 0xFFD0A020;
  uint32_t title_text_color = 0xFFFFFFFF;

  // GTK style context for header bar (owned).
  GtkStyleContext* header_ctx = nullptr;

  Impl() { InitGtk(); }

  ~Impl() {
    if (header_ctx) {
      g_object_unref(header_ctx);
      header_ctx = nullptr;
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void InitGtk() {
    // Initialize GTK (safe to call multiple times; no-op after first).
    gtk_init(nullptr, nullptr);

    // Build a minimal style context path that queries the header bar theme.
    auto* path = gtk_widget_path_new();
    gtk_widget_path_append_type(path, GTK_TYPE_WINDOW);
    const auto hdr_pos = gtk_widget_path_append_type(path, GTK_TYPE_HEADER_BAR);
    gtk_widget_path_iter_add_class(path, hdr_pos, "titlebar");
    gtk_widget_path_iter_add_class(path, hdr_pos, "header-bar");
    gtk_widget_path_iter_add_class(path, hdr_pos, GTK_STYLE_CLASS_DEFAULT);

    header_ctx = gtk_style_context_new();
    gtk_style_context_set_path(header_ctx, path);
    gtk_style_context_set_screen(header_ctx, gdk_screen_get_default());
    gtk_widget_path_free(path);

    RefreshColors();
  }

  void RefreshColors() {
    if (!header_ctx)
      return;

    // Extract background colour by rendering to a tiny Cairo surface and
    // sampling the result — avoids the deprecated
    // gtk_style_context_get_background_color().
    auto sample_bg = [](GtkStyleContext* ctx, GtkStateFlags state) -> uint32_t {
      gtk_style_context_set_state(ctx, state);
      cairo_surface_t* tmp =
          cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t* cr = cairo_create(tmp);
      gtk_render_background(ctx, cr, 0, 0, 1, 1);
      cairo_destroy(cr);
      cairo_surface_flush(tmp);
      const auto* px =
          reinterpret_cast<const uint32_t*>(cairo_image_surface_get_data(tmp));
      const uint32_t argb = px ? *px : 0u;
      cairo_surface_destroy(tmp);
      return argb;
    };

    // Active state colours.
    title_bar_color = sample_bg(header_ctx, GTK_STATE_FLAG_NORMAL);
    if ((title_bar_color & 0x00FFFFFFu) == 0)
      title_bar_color = 0xFF3C3C3C;  // Fallback if theme returns black.

    // Backdrop (unfocused) state.
    title_bar_unfocused_color = sample_bg(header_ctx, GTK_STATE_FLAG_BACKDROP);
    if ((title_bar_unfocused_color & 0x00FFFFFFu) == 0)
      title_bar_unfocused_color = 0xFF505050;

    // Text colour.
    gtk_style_context_set_state(header_ctx, GTK_STATE_FLAG_NORMAL);
    {
      GdkRGBA fg{};
      gtk_style_context_get_color(header_ctx, GTK_STATE_FLAG_NORMAL, &fg);
      title_text_color = GdkColorToArgb(fg);
    }

    // Border colour — derive from title bar with reduced brightness.
    border_color = DarkenColor(title_bar_color, 0.7);

    // Button colours — we keep sensible defaults since GTK doesn't
    // expose per-button colours in a portable way.
    close_btn_color = 0xFFE04040;
    max_btn_color = 0xFF40A040;
    min_btn_color = 0xFFD0A020;

    // Reset state.
    gtk_style_context_set_state(header_ctx, GTK_STATE_FLAG_NORMAL);
  }

  static uint32_t DarkenColor(uint32_t argb, double factor) noexcept {
    const auto a = argb & 0xFF000000u;
    auto r = static_cast<uint32_t>(((argb >> 16u) & 0xFFu) * factor);
    auto g = static_cast<uint32_t>(((argb >> 8u) & 0xFFu) * factor);
    auto b = static_cast<uint32_t>((argb & 0xFFu) * factor);
    r = std::min(r, 255u);
    g = std::min(g, 255u);
    b = std::min(b, 255u);
    return a | (r << 16u) | (g << 8u) | b;
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// GtkCsdPlugin — public interface implementation
// ══════════════════════════════════════════════════════════════════════════════

GtkCsdPlugin::GtkCsdPlugin() : impl_(std::make_unique<Impl>()) {}

GtkCsdPlugin::~GtkCsdPlugin() = default;

GtkCsdPlugin::GtkCsdPlugin(GtkCsdPlugin&&) noexcept = default;
GtkCsdPlugin& GtkCsdPlugin::operator=(GtkCsdPlugin&&) noexcept = default;

int GtkCsdPlugin::BorderWidth() const noexcept {
  return kBorderWidth;
}

int GtkCsdPlugin::TitleBarHeight() const noexcept {
  return kTitleBarHeight;
}

void GtkCsdPlugin::SetTitle(std::string_view title) {
  impl_->title = std::string{title};
}

void GtkCsdPlugin::SetState(bool focused, bool maximized) {
  impl_->focused = focused;
  impl_->maximized = maximized;
}

// ── Pixel helpers ───────────────────────────────────────────────────────────

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

static void PaintContent(uint32_t* pixels,
                         int width,
                         int height,
                         int stride,
                         uint32_t time) noexcept {
  const int halfh = height / 2;
  const int halfw = width / 2;
  int outer_r = (halfw < halfh ? halfw : halfh) - 8;
  const int inner_r = outer_r - 32;
  outer_r *= outer_r;
  const int inner_r2 = inner_r * inner_r;

  for (int y = 0; y < height; ++y) {
    const int y2 = (y - halfh) * (y - halfh);
    for (int x = 0; x < width; ++x) {
      uint32_t v;
      const int r2 = (x - halfw) * (x - halfw) + y2;
      if (r2 < inner_r2)
        v = (static_cast<uint32_t>(r2 / 32) + time / 64) * 0x0080401u;
      else if (r2 < outer_r)
        v = (static_cast<uint32_t>(y) + time / 32) * 0x0080401u;
      else
        v = (static_cast<uint32_t>(x) + time / 16) * 0x0080401u;
      v &= 0x00FFFFFFu;
      if (std::abs(x - y) > 6 && std::abs(x + y - height) > 6)
        v |= 0xFF000000u;
      pixels[y * stride + x] = v;
    }
  }
}

// ── Render title text using Pango + Cairo → XRGB8888 ────────────────────────

static void RenderTitle(uint32_t* buf,
                        int buf_w,
                        int x,
                        int y,
                        int max_w,
                        int max_h,
                        const std::string& title,
                        uint32_t text_color) noexcept {
  if (title.empty() || max_w <= 0 || max_h <= 0)
    return;

  // Create a Cairo image surface sized to the title bar text area.
  cairo_surface_t* surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, max_w, max_h);
  cairo_t* cr = cairo_create(surface);

  // Set text colour.
  const double r = ((text_color >> 16u) & 0xFFu) / 255.0;
  const double g = ((text_color >> 8u) & 0xFFu) / 255.0;
  const double b = (text_color & 0xFFu) / 255.0;
  cairo_set_source_rgb(cr, r, g, b);

  // Layout with Pango.
  PangoLayout* layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, title.c_str(), -1);

  PangoFontDescription* font = pango_font_description_from_string("Sans 11");
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);

  pango_layout_set_width(layout, max_w * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

  // Centre vertically.
  int text_w = 0;
  int text_h = 0;
  pango_layout_get_pixel_size(layout, &text_w, &text_h);
  const int offset_y = std::max(0, (max_h - text_h) / 2);
  cairo_move_to(cr, 0, offset_y);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  cairo_destroy(cr);

  // Blit the ARGB32 Cairo surface into our XRGB8888 buffer.
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

// ── Draw a circular button (close/max/min) ──────────────────────────────────

static void DrawCircleButton(uint32_t* buf,
                             int buf_w,
                             int cx,
                             int cy,
                             int radius,
                             uint32_t color) noexcept {
  const int r2 = radius * radius;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy <= r2) {
        const int px = cx + dx;
        const int py = cy + dy;
        if (px >= 0 && py >= 0)
          buf[py * buf_w + px] = color;
      }
    }
  }
}

// ── RenderFrame ─────────────────────────────────────────────────────────────

void GtkCsdPlugin::RenderFrame(uint32_t* buffer,
                               int surface_w,
                               int surface_h,
                               int content_w,
                               int content_h,
                               uint32_t time) {
  // Fill entire surface with border colour.
  FillRect(buffer, surface_w, 0, 0, surface_w, surface_h, impl_->border_color);

  // Title bar background.
  const uint32_t tb_color = impl_->focused ? impl_->title_bar_color
                                           : impl_->title_bar_unfocused_color;
  FillRect(buffer, surface_w, kBorderWidth, kBorderWidth, content_w,
           kTitleBarHeight - kBorderWidth, tb_color);

  // Window control buttons (circular, like GNOME/Adwaita).
  const int btn_cy = kBorderWidth + (kTitleBarHeight - kBorderWidth) / 2;
  const int btn_radius = kButtonSize / 2;

  // Close button (rightmost).
  int btn_cx = kBorderWidth + content_w - kButtonPadding - btn_radius;
  DrawCircleButton(buffer, surface_w, btn_cx, btn_cy, btn_radius,
                   impl_->close_btn_color);

  // Maximize button.
  btn_cx -= (kButtonSize + kButtonPadding);
  DrawCircleButton(buffer, surface_w, btn_cx, btn_cy, btn_radius,
                   impl_->max_btn_color);

  // Minimize button.
  btn_cx -= (kButtonSize + kButtonPadding);
  DrawCircleButton(buffer, surface_w, btn_cx, btn_cy, btn_radius,
                   impl_->min_btn_color);

  // Title text (rendered via Pango + Cairo).
  const int text_x = kBorderWidth + kButtonPadding;
  const int text_y = kBorderWidth;
  const int text_max_w =
      content_w - 3 * (kButtonSize + kButtonPadding) - 2 * kButtonPadding;
  const int text_max_h = kTitleBarHeight - kBorderWidth;
  if (text_max_w > 0) {
    RenderTitle(buffer, surface_w, text_x, text_y, text_max_w, text_max_h,
                impl_->title, impl_->title_text_color);
  }

  // Content area — animated ring pattern.
  uint32_t* content_start = buffer + kTitleBarHeight * surface_w + kBorderWidth;
  PaintContent(content_start, content_w, content_h, surface_w, time);
}

// ── HitTest ─────────────────────────────────────────────────────────────────

HitZone GtkCsdPlugin::HitTest(int x,
                              int y,
                              int surface_w,
                              int surface_h,
                              int content_w,
                              int /*content_h*/) const noexcept {
  if (x < 0 || y < 0 || x >= surface_w || y >= surface_h)
    return HitZone::None;

  // Corner resize zones.
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
    const int btn_cy = kBorderWidth + (kTitleBarHeight - kBorderWidth) / 2;
    const int btn_radius = kButtonSize / 2;

    // Close button.
    int btn_cx = kBorderWidth + content_w - kButtonPadding - btn_radius;
    {
      const int dx = x - btn_cx;
      const int dy = y - btn_cy;
      if (dx * dx + dy * dy <= btn_radius * btn_radius)
        return HitZone::CloseButton;
    }

    // Maximize button.
    btn_cx -= (kButtonSize + kButtonPadding);
    {
      const int dx = x - btn_cx;
      const int dy = y - btn_cy;
      if (dx * dx + dy * dy <= btn_radius * btn_radius)
        return HitZone::MaximizeButton;
    }

    // Minimize button.
    btn_cx -= (kButtonSize + kButtonPadding);
    {
      const int dx = x - btn_cx;
      const int dy = y - btn_cy;
      if (dx * dx + dy * dy <= btn_radius * btn_radius)
        return HitZone::MinimizeButton;
    }

    return HitZone::TitleBar;
  }

  return HitZone::Content;
}

}  // namespace wl::csd

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-avoid-non-const-global-variables)
