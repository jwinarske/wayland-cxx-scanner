// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_common — Shared CSD color helpers for Cairo-based plugins.
//
// ── Provided utilities
// ─────────────────────────────────────────────────────────────
//
// wl::csd::common::Rgba32ToComponents — ARGB → (r,g,b,a) doubles
// wl::csd::common::CairoSetRgba32     — set Cairo source from ARGB
#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <tuple>

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

}  // namespace wl::csd::common
