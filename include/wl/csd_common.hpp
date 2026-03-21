// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_common — Shared CSD utilities for Cairo-based plugins.
//
// Header-only helpers used by both the Cairo and GTK decoration plugins.
// Provides Gaussian blur for shadow rendering, shadow tile compositing,
// and ARGB color decomposition — ported from libdecor's
// src/plugins/common/libdecor-cairo-blur.{c,h}.
//
// ── Provided utilities
// ─────────────────────────────────────────────────────────────
//
// wl::csd::common::BlurSurface      — separable Gaussian blur
// wl::csd::common::RenderShadow     — nine-patch shadow compositing
// wl::csd::common::Rgba32ToComponents — ARGB → (r,g,b,a) doubles
// wl::csd::common::CairoSetRgba32    — set Cairo source from ARGB
#pragma once

#include <cairo/cairo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <vector>

namespace wl::csd::common {

// ══════════════════════════════════════════════════════════════════════════════
// Color helpers
// ══════════════════════════════════════════════════════════════════════════════

/// Decompose an ARGB8888 value into (r, g, b, a) doubles in [0, 1].
[[nodiscard]] inline std::tuple<double, double, double, double>
Rgba32ToComponents(uint32_t argb) noexcept {
  const double a = static_cast<double>((argb >> 24u) & 0xFFu) / 255.0;
  const double r = static_cast<double>((argb >> 16u) & 0xFFu) / 255.0;
  const double g = static_cast<double>((argb >> 8u) & 0xFFu) / 255.0;
  const double b = static_cast<double>(argb & 0xFFu) / 255.0;
  return {r, g, b, a};
}

/// Set the Cairo source color from an ARGB8888 value.
inline void CairoSetRgba32(cairo_t* cr, uint32_t argb) noexcept {
  const auto [r, g, b, a] = Rgba32ToComponents(argb);
  cairo_set_source_rgba(cr, r, g, b, a);
}

// ══════════════════════════════════════════════════════════════════════════════
// Gaussian blur — port of libdecor's blur_surface()
// ══════════════════════════════════════════════════════════════════════════════

/// Apply a two-pass separable Gaussian blur to a Cairo ARGB32 surface.
///
/// Uses a fixed 71-element kernel and only processes pixels within
/// @p margin of the surface edges (interior pixels are left unchanged).
/// A temporary buffer is allocated internally for the intermediate pass.
///
/// @returns 0 on success, -1 on failure (wrong format / null data).
[[nodiscard]] inline int BlurSurface(cairo_surface_t* surface, int margin) {
  if (cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
    return -1;

  cairo_surface_flush(surface);

  const int w = cairo_image_surface_get_width(surface);
  const int h = cairo_image_surface_get_height(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  auto* data = cairo_image_surface_get_data(surface);

  if (data == nullptr || w <= 0 || h <= 0)
    return -1;

  // ── Build 71-element Gaussian kernel ────────────────────────────────────
  constexpr int kSize = 71;
  constexpr int kHalf = kSize / 2;
  const double sigma = static_cast<double>(kSize) / 3.0;

  std::array<double, kSize> kernel{};
  double total = 0.0;
  for (int i = 0; i < kSize; ++i) {
    const double x = static_cast<double>(i - kHalf);
    kernel[static_cast<std::size_t>(i)] =
        std::exp(-(x * x) / (2.0 * sigma * sigma));
    total += kernel[static_cast<std::size_t>(i)];
  }
  for (auto& k : kernel)
    k /= total;

  // Predicate: true if pixel is within margin of any edge.
  auto near_edge = [&](int col, int row) noexcept {
    return col < margin || col >= w - margin || row < margin ||
           row >= h - margin;
  };

  // ── Temporary buffer ────────────────────────────────────────────────────
  const auto buf_bytes =
      static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
  std::vector<uint8_t> tmp(buf_bytes);
  std::memcpy(tmp.data(), data, buf_bytes);

  // ── Horizontal pass: data → tmp ─────────────────────────────────────────
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      if (!near_edge(col, row))
        continue;

      double rb = 0.0;
      double gb = 0.0;
      double bb = 0.0;
      double ab = 0.0;
      for (int k = 0; k < kSize; ++k) {
        const int sc = std::clamp(col + k - kHalf, 0, w - 1);
        const auto* px = data + row * stride + sc * 4;
        const double weight = kernel[static_cast<std::size_t>(k)];
        bb += weight * px[0];
        gb += weight * px[1];
        rb += weight * px[2];
        ab += weight * px[3];
      }
      auto* dst = tmp.data() + row * stride + col * 4;
      dst[0] = static_cast<uint8_t>(std::clamp(bb, 0.0, 255.0));
      dst[1] = static_cast<uint8_t>(std::clamp(gb, 0.0, 255.0));
      dst[2] = static_cast<uint8_t>(std::clamp(rb, 0.0, 255.0));
      dst[3] = static_cast<uint8_t>(std::clamp(ab, 0.0, 255.0));
    }
  }

  // ── Vertical pass: tmp → data ───────────────────────────────────────────
  for (int col = 0; col < w; ++col) {
    for (int row = 0; row < h; ++row) {
      if (!near_edge(col, row))
        continue;

      double rb = 0.0;
      double gb = 0.0;
      double bb = 0.0;
      double ab = 0.0;
      for (int k = 0; k < kSize; ++k) {
        const int sr = std::clamp(row + k - kHalf, 0, h - 1);
        const auto* px = tmp.data() + sr * stride + col * 4;
        const double weight = kernel[static_cast<std::size_t>(k)];
        bb += weight * px[0];
        gb += weight * px[1];
        rb += weight * px[2];
        ab += weight * px[3];
      }
      auto* dst = data + row * stride + col * 4;
      dst[0] = static_cast<uint8_t>(std::clamp(bb, 0.0, 255.0));
      dst[1] = static_cast<uint8_t>(std::clamp(gb, 0.0, 255.0));
      dst[2] = static_cast<uint8_t>(std::clamp(rb, 0.0, 255.0));
      dst[3] = static_cast<uint8_t>(std::clamp(ab, 0.0, 255.0));
    }
  }

  cairo_surface_mark_dirty(surface);
  return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
// Shadow rendering — port of libdecor's render_shadow()
// ══════════════════════════════════════════════════════════════════════════════

/// Render a soft shadow around a window rectangle using a pre-blurred tile.
///
/// The tile is split into a nine-patch: four corners are painted 1 : 1,
/// the four edges are horizontally or vertically stretched, and the centre
/// is left empty (the window content).  The tile is used as both source
/// and mask via @c cairo_mask so that the shadow alpha composites correctly.
///
/// @param cr           Destination Cairo context.
/// @param shadow_tile  Pre-blurred ARGB32 tile surface.
/// @param x            Left edge of the inner window rectangle.
/// @param y            Top edge of the inner window rectangle.
/// @param width        Width of the inner window rectangle.
/// @param height       Height of the inner window rectangle.
/// @param margin       Shadow extent on left / right / bottom.
/// @param top_margin   Shadow extent on top.
inline void RenderShadow(cairo_t* cr,
                         cairo_surface_t* shadow_tile,
                         int x,
                         int y,
                         int width,
                         int height,
                         int margin,
                         int top_margin) {
  const int tw = cairo_image_surface_get_width(shadow_tile);
  const int th = cairo_image_surface_get_height(shadow_tile);
  const int inner_w = tw - 2 * margin;
  const int inner_h = th - top_margin - margin;

  // Helper: paint a (possibly stretched) region of the tile as both
  // source and mask so the alpha channel composites correctly.
  auto paint_region = [&](double dst_x, double dst_y, double dst_w,
                          double dst_h, double src_x, double src_y,
                          double src_w, double src_h) {
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
      return;

    cairo_save(cr);
    cairo_rectangle(cr, dst_x, dst_y, dst_w, dst_h);
    cairo_clip(cr);

    cairo_pattern_t* pat = cairo_pattern_create_for_surface(shadow_tile);
    cairo_matrix_t mat;
    const double sx = src_w / dst_w;
    const double sy = src_h / dst_h;
    cairo_matrix_init(&mat, sx, 0, 0, sy, src_x - sx * dst_x,
                      src_y - sy * dst_y);
    cairo_pattern_set_matrix(pat, &mat);

    cairo_set_source(cr, pat);
    cairo_mask(cr, pat);
    cairo_pattern_destroy(pat);
    cairo_restore(cr);
  };

  const auto dx = static_cast<double>(x);
  const auto dy = static_cast<double>(y);
  const auto dw = static_cast<double>(width);
  const auto dh = static_cast<double>(height);
  const auto dm = static_cast<double>(margin);
  const auto dtm = static_cast<double>(top_margin);
  const auto dtw = static_cast<double>(tw);
  const auto dth = static_cast<double>(th);
  const auto diw = static_cast<double>(inner_w);
  const auto dih = static_cast<double>(inner_h);

  // ── Four corners (1 : 1 mapping) ──────────────────────────────────────
  paint_region(dx - dm, dy - dtm, dm, dtm, 0, 0, dm, dtm);
  paint_region(dx + dw, dy - dtm, dm, dtm, dtw - dm, 0, dm, dtm);
  paint_region(dx - dm, dy + dh, dm, dm, 0, dth - dm, dm, dm);
  paint_region(dx + dw, dy + dh, dm, dm, dtw - dm, dth - dm, dm, dm);

  // ── Edges (stretched) ─────────────────────────────────────────────────
  paint_region(dx, dy - dtm, dw, dtm, dm, 0, diw, dtm);
  paint_region(dx, dy + dh, dw, dm, dm, dth - dm, diw, dm);
  paint_region(dx - dm, dy, dm, dh, 0, dtm, dm, dih);
  paint_region(dx + dw, dy, dm, dh, dtw - dm, dtm, dm, dih);
}

}  // namespace wl::csd::common
