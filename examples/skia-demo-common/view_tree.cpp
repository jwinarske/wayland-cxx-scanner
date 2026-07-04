// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors

#include "view_tree.hpp"

#include <algorithm>

namespace demo {

namespace {

constexpr SkScalar kMargin = 16.0F;
constexpr SkScalar kSpinnerRadius = 26.0F;
constexpr SkScalar kSpinnerStroke = 5.0F;

}  // namespace

void ViewTree::Layout(int width, int height) noexcept {
  if (width == last_width_ && height == last_height_)
    return;
  last_width_ = width;
  last_height_ = height;

  panel_ =
      SkRect::MakeLTRB(kMargin, kMargin, static_cast<SkScalar>(width) - kMargin,
                       static_cast<SkScalar>(height) - kMargin);

  bounds_.at(Index(View::kCard)) =
      SkRect::MakeXYWH(panel_.left() + 24.0F, panel_.top() + 24.0F,
                       panel_.width() - 48.0F, 96.0F);

  bounds_.at(Index(View::kButton)) = SkRect::MakeXYWH(
      panel_.left() + 24.0F, panel_.bottom() - 72.0F, 132.0F, 44.0F);

  const SkPoint spinner_center = {panel_.right() - 56.0F,
                                  panel_.bottom() - 50.0F};
  const SkScalar r = kSpinnerRadius + kSpinnerStroke;
  bounds_.at(Index(View::kSpinner)) =
      SkRect::MakeLTRB(spinner_center.fX - r, spinner_center.fY - r,
                       spinner_center.fX + r, spinner_center.fY + r);
}

std::optional<View> ViewTree::HitTest(SkScalar x, SkScalar y) const noexcept {
  // Front-to-back: the spinner draws on top, the card at the back.  A static
  // order table avoids materializing an initializer_list on every call.
  static constexpr std::array<View, 3> kFrontToBack = {
      View::kSpinner, View::kButton, View::kCard};
  for (View v : kFrontToBack) {
    if (bounds_.at(Index(v)).contains(x, y))
      return v;
  }
  return std::nullopt;
}

bool ViewTree::AnyDirty() const noexcept {
  return std::any_of(dirty_.begin(), dirty_.end(), [](bool d) { return d; });
}

void ViewTree::CollectDamage(std::vector<SkIRect>& out) const noexcept {
  for (std::size_t i = 0; i < kN; ++i) {
    if (dirty_.at(i))
      out.push_back(bounds_.at(i).roundOut());
  }
}

}  // namespace demo
