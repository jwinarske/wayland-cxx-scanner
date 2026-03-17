// SPDX-License-Identifier: MIT
// Tests for wl::DisplayHandle.
// These tests exercise null/error-path logic without a live Wayland compositor.
#include <wl/display.hpp>

#include <gtest/gtest.h>

// ── DisplayHandle tests ───────────────────────────────────────────────────────

TEST(DisplayHandle, DefaultIsNull) {
  wl::DisplayHandle dh;
  EXPECT_TRUE(dh.IsNull());
}

TEST(DisplayHandle, GetReturnsNullWhenDisconnected) {
  wl::DisplayHandle dh;
  EXPECT_EQ(dh.Get(), nullptr);
}

TEST(DisplayHandle, BoolConversionFalseWhenDisconnected) {
  wl::DisplayHandle dh;
  EXPECT_FALSE(static_cast<bool>(dh));
}

TEST(DisplayHandle, ConnectToNonexistentDisplayReturnsFalse) {
  // Use a name that is guaranteed not to exist so Connect() returns false.
  wl::DisplayHandle dh;
  EXPECT_FALSE(dh.Connect("nonexistent-wayland-display-xyz-99999"));
  EXPECT_TRUE(dh.IsNull());
}

TEST(DisplayHandle, DisconnectOnNullIsSafe) {
  // Disconnect() on a null handle must be a no-op and not crash.
  wl::DisplayHandle dh;
  dh.Disconnect();
  EXPECT_TRUE(dh.IsNull());
}

TEST(DisplayHandle, RepeatedDisconnectIsSafe) {
  // Calling Disconnect() multiple times on a null handle must not crash.
  wl::DisplayHandle dh;
  dh.Disconnect();
  dh.Disconnect();
  EXPECT_TRUE(dh.IsNull());
}
