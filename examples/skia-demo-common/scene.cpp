// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// DemoScene implementation.  Core Skia only (no module dependencies yet): the
// rich text card built on SkParagraph and the bundled deterministic fonts land
// together with the golden-image harness.  What is here already exercises the
// paths that matter for the raster backend: rounded-rect clipping, a gradient
// shader, and a dashed-arc path effect.

#include "scene.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTileMode.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"

#include <array>

namespace demo {

namespace {

constexpr SkScalar kMargin = 16.0F;
constexpr SkScalar kPanelRadius = 18.0F;
constexpr SkScalar kButtonRadius = 10.0F;

// A rounded-rect panel filling the window inset by the margin, painted with a
// vertical linear gradient.  Exercises clip + shader together.
void DrawBackgroundPanel(SkCanvas* canvas, const SkRect& panel) noexcept {
  const std::array<SkColor4f, 2> colors = {
      SkColor4f::FromColor(SkColorSetRGB(0x1E, 0x22, 0x2B)),
      SkColor4f::FromColor(SkColorSetRGB(0x2C, 0x35, 0x48))};
  const std::array<SkPoint, 2> pts = {SkPoint{panel.centerX(), panel.top()},
                                      SkPoint{panel.centerX(), panel.bottom()}};

  const SkGradient::Colors gradient_colors(
      SkSpan<const SkColor4f>(colors.data(), colors.size()),
      SkTileMode::kClamp);
  const SkGradient gradient(gradient_colors, SkGradient::Interpolation{});

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setDither(false);  // raster vs GPU gradient dithering must not diverge
  paint.setShader(SkShaders::LinearGradient(pts.data(), gradient));

  const SkRRect rrect = SkRRect::MakeRectXY(panel, kPanelRadius, kPanelRadius);
  canvas->drawRRect(rrect, paint);
}

// A state-dependent button in the lower-left of the panel.
void DrawButton(SkCanvas* canvas, const SkRect& panel, bool active) noexcept {
  const SkRect rect = SkRect::MakeXYWH(panel.left() + 24.0F,
                                       panel.bottom() - 72.0F, 132.0F, 44.0F);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(active ? SkColorSetRGB(0x4C, 0x9A, 0xFF)
                        : SkColorSetRGB(0x35, 0x40, 0x55));
  canvas->drawRRect(SkRRect::MakeRectXY(rect, kButtonRadius, kButtonRadius),
                    paint);
}

// Placeholder for the text card.  Replaced by an SkParagraph-rendered card
// (mixed-weight runs, emoji, an RTL span) when the deterministic font stack
// and golden harness are introduced.
void DrawTextCard(SkCanvas* canvas, const SkRect& panel) noexcept {
  const SkRect rect = SkRect::MakeXYWH(
      panel.left() + 24.0F, panel.top() + 24.0F, panel.width() - 48.0F, 96.0F);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(SkColorSetARGB(0x22, 0xFF, 0xFF, 0xFF));
  canvas->drawRRect(SkRRect::MakeRectXY(rect, 8.0F, 8.0F), paint);
}

// A rotating dashed-stroke arc — the classic path-effect corner case.
void DrawSpinner(SkCanvas* canvas,
                 const SkRect& panel,
                 std::uint32_t frame) noexcept {
  const SkPoint center = {panel.right() - 56.0F, panel.bottom() - 50.0F};
  constexpr SkScalar kRadius = 26.0F;

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(5.0F);
  paint.setStrokeCap(SkPaint::kRound_Cap);
  paint.setColor(SkColorSetRGB(0xF2, 0xC4, 0x4C));

  const std::array<SkScalar, 2> intervals = {14.0F, 8.0F};
  paint.setPathEffect(SkDashPathEffect::Make(
      SkSpan<const SkScalar>(intervals.data(), intervals.size()), 0.0F));

  // One revolution every 120 frames; deterministic in the frame counter so a
  // fixed timestep reproduces byte-identical output.
  const SkScalar angle =
      static_cast<SkScalar>(frame % 120u) * (360.0F / 120.0F);

  canvas->save();
  canvas->rotate(angle, center.x(), center.y());
  SkPathBuilder builder;
  builder.addArc(SkRect::MakeLTRB(center.x() - kRadius, center.y() - kRadius,
                                  center.x() + kRadius, center.y() + kRadius),
                 0.0F, 300.0F);
  canvas->drawPath(builder.detach(), paint);
  canvas->restore();
}

}  // namespace

void DemoScene::Render(SkCanvas* canvas,
                       const SceneState& state,
                       SkIRect* out_damage) noexcept {
  canvas->clear(SkColorSetRGB(0x12, 0x14, 0x18));

  const SkRect panel = SkRect::MakeLTRB(
      kMargin, kMargin, static_cast<SkScalar>(state.width) - kMargin,
      static_cast<SkScalar>(state.height) - kMargin);

  DrawBackgroundPanel(canvas, panel);
  DrawTextCard(canvas, panel);
  DrawButton(canvas, panel, state.button_active);
  DrawSpinner(canvas, panel, state.frame);

  if (out_damage != nullptr) {
    // Full-scene damage for now; refined to the union of changed views once
    // dirty-rect tracking is wired through the view tree.
    *out_damage = SkIRect::MakeWH(state.width, state.height);
  }
}

}  // namespace demo
