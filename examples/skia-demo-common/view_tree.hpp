// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// ViewTree — the single source of the demo scene's view geometry.
//
// A small, flat set of named views laid out from the logical window size.  The
// scene draws each view at its bounds, the example hit-tests pointer
// coordinates against it, and per-frame damage is the union of the views marked
// dirty.  Keeping all three uses on one layout guarantees they never disagree.
//
// Pure geometry (Skia rectangles only, no Wayland), so it is unit-testable.

#pragma once

#include "include/core/SkRect.h"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace demo {

enum class View : std::size_t {
  kCard,
  kButton,
  kSpinner,
  kCount,
};

class ViewTree {
 public:
  ViewTree() noexcept { ClearDirty(); }

  // Recomputes every view's bounds for a logical window size (pixels).  Cheap
  // to call every frame: a repeat call with the same size is a no-op.
  void Layout(int width, int height) noexcept;

  [[nodiscard]] const SkRect& Bounds(View v) const noexcept {
    return bounds_.at(Index(v));
  }
  // The background panel behind the views (not itself a hit-testable view).
  [[nodiscard]] const SkRect& Panel() const noexcept { return panel_; }

  // Topmost (front-most) view containing (x, y) in logical pixels, or
  // std::nullopt when the point is over no view.
  [[nodiscard]] std::optional<View> HitTest(SkScalar x,
                                            SkScalar y) const noexcept;

  void MarkDirty(View v) noexcept { dirty_.at(Index(v)) = true; }
  void ClearDirty() noexcept { dirty_.fill(false); }
  [[nodiscard]] bool AnyDirty() const noexcept;

  // Appends the rounded-out bounds of every dirty view to `out`.
  void CollectDamage(std::vector<SkIRect>& out) const noexcept;

 private:
  static constexpr std::size_t kN = static_cast<std::size_t>(View::kCount);
  static std::size_t Index(View v) noexcept {
    return static_cast<std::size_t>(v);
  }

  // Last size passed to Layout, so a repeat call with the same size skips the
  // recompute.  Seeded out of range so the first Layout always runs.
  int last_width_ = -1;
  int last_height_ = -1;

  SkRect panel_ = SkRect::MakeEmpty();
  std::array<SkRect, kN> bounds_{};
  std::array<bool, kN> dirty_{};
};

}  // namespace demo
