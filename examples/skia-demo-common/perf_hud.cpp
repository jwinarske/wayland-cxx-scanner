// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors

#include "perf_hud.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "frame_pacer.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkPaint.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"

namespace demo {

void FpsMeter::Tick(double now_ms) noexcept {
  // Drop stamps that have aged out of the window.
  const double cutoff = now_ms - kWindowMs;
  while (count_ > 0 && stamps_.at(head_) < cutoff) {
    head_ = (head_ + 1) % kCapacity;
    --count_;
  }
  // Ring full (frames faster than kCapacity within the window): drop the oldest
  // so the newest still lands.  fps() then saturates at kCapacity.
  if (count_ == kCapacity) {
    head_ = (head_ + 1) % kCapacity;
    --count_;
  }
  stamps_.at((head_ + count_) % kCapacity) = now_ms;
  ++count_;
}

void PerfHud::EnsureFont() {
  if (font_ready_)
    return;
  // Monospace keeps the changing numbers from jittering horizontally.  A null
  // match falls back to SkFont's default typeface rather than failing.
  const sk_sp<SkFontMgr> mgr =
      SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
  sk_sp<SkTypeface> face;
  if (mgr != nullptr)
    face = mgr->matchFamilyStyle("monospace", SkFontStyle());
  font_ = SkFont(face, kFontSize);
  font_.setEdging(SkFont::Edging::kAntiAlias);
  font_ready_ = true;
}

void PerfHud::Render(SkCanvas* canvas, const FramePacer& pacer, double fps) {
  if (!visible_ || canvas == nullptr)
    return;
  EnsureFont();

  const SkRect panel = SkRect::Make(Bounds());
  SkPaint bg;
  bg.setColor(SkColorSetARGB(0xC0, 0x10, 0x14, 0x1C));
  bg.setAntiAlias(true);
  canvas->drawRoundRect(panel, 4.0F, 4.0F, bg);

  SkPaint text;
  text.setColor(SK_ColorWHITE);
  text.setAntiAlias(true);

  std::array<char, 96> line{};
  const auto x = static_cast<float>(kOriginX + kPad);
  auto baseline = static_cast<float>(kOriginY + kPad) + kFontSize;
  const auto draw = [&] {
    canvas->drawSimpleText(line.data(), std::strlen(line.data()),
                           SkTextEncoding::kUTF8, x, baseline, font_, text);
    baseline += static_cast<float>(kLineH);
  };

  std::snprintf(line.data(), line.size(), "FPS %3.0f   frame %u", fps,
                pacer.frame());
  draw();

  if (pacer.present_count() > 0) {
    std::snprintf(line.data(), line.size(), "lat %5.2f ms  p95 %5.2f",
                  pacer.PresentMean(), pacer.PresentPercentile(95));
  } else {
    std::snprintf(line.data(), line.size(), "lat    n/a  (no wp_presentation)");
  }
  draw();

  const double hz = pacer.refresh_hz();
  if (hz > 0.0)
    std::snprintf(line.data(), line.size(), "refresh %6.2f Hz", hz);
  else
    std::snprintf(line.data(), line.size(), "refresh    n/a");
  draw();
}

}  // namespace demo
