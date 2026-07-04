// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// DemoScene implementation.  Exercises the paths that matter for the raster
// backend: rounded-rect clipping, a gradient shader, a dashed-arc path effect,
// and a mixed-script text card laid out with SkParagraph.
//
// The text card resolves fonts through the system fontconfig manager, so its
// glyphs are not reproducible across machines.  Golden-image tests render the
// scene with the text card's glyphs suppressed; everything else is
// deterministic.

#include "scene.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkSpan.h"
#include "include/core/SkString.h"
#include "include/core/SkTileMode.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#include "modules/skunicode/include/SkUnicode.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

#include <array>
#include <vector>

namespace demo {

namespace {

constexpr SkScalar kMargin = 16.0F;
constexpr SkScalar kPanelRadius = 18.0F;
constexpr SkScalar kButtonRadius = 10.0F;
constexpr SkScalar kSpinnerRadius = 26.0F;
constexpr SkScalar kSpinnerStroke = 5.0F;

// Geometry helpers shared by the drawing and the damage reporting so the two
// never disagree about where a view is.
SkRect PanelRect(int width, int height) noexcept {
  return SkRect::MakeLTRB(kMargin, kMargin,
                          static_cast<SkScalar>(width) - kMargin,
                          static_cast<SkScalar>(height) - kMargin);
}

SkRect ButtonRect(const SkRect& panel) noexcept {
  return SkRect::MakeXYWH(panel.left() + 24.0F, panel.bottom() - 72.0F, 132.0F,
                          44.0F);
}

SkPoint SpinnerCenter(const SkRect& panel) noexcept {
  return {panel.right() - 56.0F, panel.bottom() - 50.0F};
}

// Bounds of the spinner including its stroke width and round caps.
SkRect SpinnerBounds(const SkRect& panel) noexcept {
  const SkPoint c = SpinnerCenter(panel);
  const SkScalar r = kSpinnerRadius + kSpinnerStroke;
  return SkRect::MakeLTRB(c.fX - r, c.fY - r, c.fX + r, c.fY + r);
}

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
void DrawButton(SkCanvas* canvas, const SkRect& rect, bool active) noexcept {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(active ? SkColorSetRGB(0x4C, 0x9A, 0xFF)
                        : SkColorSetRGB(0x35, 0x40, 0x55));
  canvas->drawRRect(SkRRect::MakeRectXY(rect, kButtonRadius, kButtonRadius),
                    paint);
}

// A text card laid out with SkParagraph: mixed-weight runs, a color-emoji
// fallback, and an RTL span, over a translucent rounded panel.  This is the
// primary reason Skia's text stack (SkParagraph / SkShaper / SkUnicode) is
// exercised here.
//
// Fonts come from the system fontconfig manager, so the glyphs are not
// reproducible across machines; `with_text` lets golden-image tests draw just
// the (deterministic) card panel.
void DrawTextCard(SkCanvas* canvas,
                  const SkRect& panel,
                  bool with_text) noexcept {
  namespace para = skia::textlayout;

  const SkRect rect = SkRect::MakeXYWH(
      panel.left() + 24.0F, panel.top() + 24.0F, panel.width() - 48.0F, 96.0F);

  SkPaint bg;
  bg.setAntiAlias(true);
  bg.setColor(SkColorSetARGB(0x22, 0xFF, 0xFF, 0xFF));
  canvas->drawRRect(SkRRect::MakeRectXY(rect, 8.0F, 8.0F), bg);

  if (!with_text)
    return;

  // The unicode implementation and font collection are independent of the
  // frame, so build them once.  A null result (no fontconfig / ICU) degrades
  // to a card with no text rather than a crash.
  static const sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();
  static const sk_sp<para::FontCollection> fonts = [] {
    auto collection = sk_make_sp<para::FontCollection>();
    collection->setDefaultFontManager(
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType()));
    collection->enableFontFallback();
    return collection;
  }();
  if (unicode == nullptr || fonts == nullptr)
    return;

  const para::ParagraphStyle paragraph_style;
  std::unique_ptr<para::ParagraphBuilder> builder =
      para::ParagraphBuilder::make(paragraph_style, fonts, unicode);
  if (builder == nullptr)
    return;

  const std::vector<SkString> families = {SkString("sans-serif")};
  constexpr SkScalar kTextSize = 21.0F;

  para::TextStyle heading;
  heading.setColor(SK_ColorWHITE);
  heading.setFontFamilies(families);
  heading.setFontSize(kTextSize);
  heading.setFontStyle(SkFontStyle::Bold());
  builder->pushStyle(heading);
  builder->addText("Wayland");
  builder->pop();

  para::TextStyle body;
  body.setColor(SkColorSetRGB(0xC8, 0xD2, 0xE0));
  body.setFontFamilies(families);
  body.setFontSize(kTextSize);
  builder->pushStyle(body);
  builder->addText(" + Skia ");
  builder->pop();

  // U+1F680 rocket — resolved through color-emoji fallback.
  para::TextStyle emoji;
  emoji.setFontFamilies(families);
  emoji.setFontSize(kTextSize);
  builder->pushStyle(emoji);
  builder->addText("\xF0\x9F\x9A\x80  ");
  builder->pop();

  // Arabic "marhaba" (hello) — exercises RTL shaping and bidi.
  para::TextStyle rtl;
  rtl.setColor(SkColorSetRGB(0x9E, 0xE4, 0xB0));
  rtl.setFontFamilies(families);
  rtl.setFontSize(kTextSize);
  builder->pushStyle(rtl);
  builder->addText("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");
  builder->pop();

  std::unique_ptr<para::Paragraph> paragraph = builder->Build();
  paragraph->layout(rect.width() - 24.0F);
  paragraph->paint(canvas, rect.left() + 12.0F, rect.top() + 12.0F);
}

// A rotating dashed-stroke arc — the classic path-effect corner case.
void DrawSpinner(SkCanvas* canvas,
                 const SkRect& panel,
                 std::uint32_t frame) noexcept {
  const SkPoint center = SpinnerCenter(panel);

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(kSpinnerStroke);
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
  builder.addArc(SkRect::MakeLTRB(
                     center.x() - kSpinnerRadius, center.y() - kSpinnerRadius,
                     center.x() + kSpinnerRadius, center.y() + kSpinnerRadius),
                 0.0F, 300.0F);
  canvas->drawPath(builder.detach(), paint);
  canvas->restore();
}

}  // namespace

void DemoScene::Render(SkCanvas* canvas,
                       const SceneState& state,
                       std::vector<SkIRect>* out_damage) noexcept {
  canvas->clear(SkColorSetRGB(0x12, 0x14, 0x18));

  const SkRect panel = PanelRect(state.width, state.height);
  const SkRect button = ButtonRect(panel);

  DrawBackgroundPanel(canvas, panel);
  DrawTextCard(canvas, panel, state.draw_text);
  DrawButton(canvas, button, state.button_active);
  DrawSpinner(canvas, panel, state.frame);

  if (out_damage != nullptr) {
    // The whole scene is redrawn every frame, so the reported damage is the
    // difference between consecutive frames: the animated spinner always, plus
    // the button on the frame its state changed.
    out_damage->clear();
    out_damage->push_back(SpinnerBounds(panel).roundOut());
    if (state.button_dirty)
      out_damage->push_back(button.roundOut());
  }
}

}  // namespace demo
