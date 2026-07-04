// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Unit tests for the view tree (layout, hit-testing, dirty damage).

#include "view_tree.hpp"

#include "include/core/SkRect.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

using demo::View;
using demo::ViewTree;

namespace {

ViewTree LaidOut(int w, int h) {
  ViewTree t;
  t.Layout(w, h);
  return t;
}

TEST(ViewTree, LayoutBounds) {
  const ViewTree t = LaidOut(480, 320);
  // panel = (16, 16, 464, 304).
  EXPECT_EQ(t.Panel(), SkRect::MakeLTRB(16, 16, 464, 304));
  EXPECT_EQ(t.Bounds(View::kCard), SkRect::MakeLTRB(40, 40, 440, 136));
  EXPECT_EQ(t.Bounds(View::kButton), SkRect::MakeLTRB(40, 232, 172, 276));
  EXPECT_EQ(t.Bounds(View::kSpinner), SkRect::MakeLTRB(377, 223, 439, 285));
}

TEST(ViewTree, RelayoutOnSizeChange) {
  // The memoized Layout must still recompute when the size actually changes.
  ViewTree t;
  t.Layout(480, 320);
  const SkRect spinner_480 = t.Bounds(View::kSpinner);
  t.Layout(640, 480);
  EXPECT_EQ(t.Panel(), SkRect::MakeLTRB(16, 16, 624, 464));
  EXPECT_NE(t.Bounds(View::kSpinner), spinner_480);
}

TEST(ViewTree, HitTestInsideEachView) {
  const ViewTree t = LaidOut(480, 320);
  EXPECT_EQ(t.HitTest(100, 250), std::make_optional(View::kButton));
  EXPECT_EQ(t.HitTest(408, 254), std::make_optional(View::kSpinner));
  EXPECT_EQ(t.HitTest(240, 80), std::make_optional(View::kCard));
}

TEST(ViewTree, HitTestMissIsEmpty) {
  const ViewTree t = LaidOut(480, 320);
  EXPECT_FALSE(t.HitTest(5, 5).has_value());      // outside the panel
  EXPECT_FALSE(t.HitTest(300, 250).has_value());  // between button and spinner
}

TEST(ViewTree, DirtyTrackingCollectsDamage) {
  ViewTree t = LaidOut(480, 320);
  std::vector<SkIRect> dmg;

  t.CollectDamage(dmg);
  EXPECT_TRUE(dmg.empty());
  EXPECT_FALSE(t.AnyDirty());

  t.MarkDirty(View::kSpinner);
  EXPECT_TRUE(t.AnyDirty());
  t.CollectDamage(dmg);
  ASSERT_EQ(dmg.size(), 1u);
  EXPECT_EQ(dmg[0], SkIRect::MakeLTRB(377, 223, 439, 285));

  dmg.clear();
  t.MarkDirty(View::kButton);
  t.CollectDamage(dmg);
  EXPECT_EQ(dmg.size(), 2u);

  dmg.clear();
  t.ClearDirty();
  EXPECT_FALSE(t.AnyDirty());
  t.CollectDamage(dmg);
  EXPECT_TRUE(dmg.empty());
}

}  // namespace
