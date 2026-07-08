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

void DamageMeter::Tick(double now_ms, double fraction) noexcept {
  const double clamped =
      fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
  const double cutoff = now_ms - kWindowMs;
  while (count_ > 0 && stamps_.at(head_) < cutoff) {
    head_ = (head_ + 1) % kCapacity;
    --count_;
  }
  if (count_ == kCapacity) {
    head_ = (head_ + 1) % kCapacity;
    --count_;
  }
  const std::size_t slot = (head_ + count_) % kCapacity;
  stamps_.at(slot) = now_ms;
  fracs_.at(slot) = clamped;
  ++count_;
}

double DamageMeter::mean_fraction() const noexcept {
  if (count_ == 0)
    return 0.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < count_; ++i)
    sum += fracs_.at((head_ + i) % kCapacity);
  return sum / static_cast<double>(count_);
}

void PerfHud::SetExtraLine(std::size_t idx, const char* text) noexcept {
  if (idx >= kMaxExtraLines)
    return;
  auto& slot = extra_.at(idx);
  if (text == nullptr) {
    slot[0] = '\0';
    return;
  }
  std::snprintf(slot.data(), slot.size(), "%s", text);
}

std::size_t PerfHud::ActiveExtra() const noexcept {
  std::size_t n = 0;
  for (const auto& line : extra_) {
    if (line[0] == '\0')
      break;
    ++n;
  }
  return n;
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

  // Application-specific lines (e.g. the Skottie example's commit rate and
  // damage coverage), drawn under the standard stats.
  for (std::size_t i = 0; i < ActiveExtra(); ++i) {
    std::snprintf(line.data(), line.size(), "%s", extra_.at(i).data());
    draw();
  }
}

}  // namespace demo
