// SPDX-License-Identifier: MIT
// Tests for wl::KeyboardHandler<App> (keyboard.hpp) and
// wl::SeatManager<App> (seat.hpp).
//
// wayland_client.hpp is generated from wayland.xml at build time and must be
// included first so that CWlKeyboard<>, CWlSeat<>, etc. are visible.
//
// These tests exercise handler logic without a live Wayland compositor.
// All request methods (_Marshal) are no-ops when the proxy is null.
#include "wayland_client.hpp"  // generated — must come before seat.hpp
#include <wl/seat.hpp>         // also pulls in keyboard.hpp

#include <fcntl.h>   // F_GETFD, fcntl
#include <unistd.h>  // pipe, close

#include <gtest/gtest.h>
#include <cerrno>
#include <cstdint>

// ── Minimal App stubs ─────────────────────────────────────────────────────────

// App without OnKeySym — exercises the SFINAE fallback.
struct FakeSeatApp {
  uint32_t last_key = 0;
  uint32_t last_state = 0;

  void OnKey(uint32_t key, uint32_t state) {
    last_key = key;
    last_state = state;
  }
};

// App with OnKeySym — exercises the SFINAE primary branch.
struct FakeSeatAppWithKeySym {
  uint32_t last_key = 0;
  uint32_t last_state = 0;
  xkb_keysym_t last_sym = XKB_KEY_NoSymbol;

  void OnKey(uint32_t key, uint32_t state) {
    last_key = key;
    last_state = state;
  }
  void OnKeySym(xkb_keysym_t sym, uint32_t /*key*/, uint32_t /*state*/) {
    last_sym = sym;
  }
};

// ── KeyboardHandler tests ─────────────────────────────────────────────────────

TEST(KeyboardHandler, ConstructionCreatesXkbContext) {
  // Constructor must not crash; it calls xkb_context_new internally.
  wl::KeyboardHandler<FakeSeatApp> kbd;
  EXPECT_TRUE(kbd.IsNull());
}

TEST(KeyboardHandler, OnKeyWithNullAppIsNoOp) {
  // When app_ is null, OnKey must return early and not crash.
  wl::KeyboardHandler<FakeSeatApp> kbd;
  kbd.OnKey(0u, 0u, 30u, 1u);  // must not crash
}

TEST(KeyboardHandler, OnKeyWithAppAndNullXkbStateCallsApp) {
  // When xkb_state_ is null (no keymap loaded) and app_ is set,
  // OnKey must call app_->OnKey directly without keysym translation.
  wl::KeyboardHandler<FakeSeatApp> kbd;
  FakeSeatApp app;
  kbd.app_ = &app;
  kbd.OnKey(0u, 0u, 30u, 1u);
  EXPECT_EQ(app.last_key, 30u);
  EXPECT_EQ(app.last_state, 1u);
}

TEST(KeyboardHandler, OnKeyWithAppWithKeySymAndNullXkbStateCallsApp) {
  // Same as above but with an App that provides OnKeySym.
  // With no keymap, the xkb_state_ branch is skipped so OnKeySym is NOT called.
  wl::KeyboardHandler<FakeSeatAppWithKeySym> kbd;
  FakeSeatAppWithKeySym app;
  kbd.app_ = &app;
  kbd.OnKey(0u, 0u, 30u, 1u);
  EXPECT_EQ(app.last_key, 30u);
  EXPECT_EQ(app.last_state, 1u);
  EXPECT_EQ(app.last_sym, xkb_keysym_t{XKB_KEY_NoSymbol});  // not set — no xkb_state
}

TEST(KeyboardHandler, OnModifiersWithNullXkbStateIsNoOp) {
  wl::KeyboardHandler<FakeSeatApp> kbd;
  kbd.OnModifiers(0u, 0u, 0u, 0u, 0u);  // must not crash
}

TEST(KeyboardHandler, OnEnterIsNoOp) {
  wl::KeyboardHandler<FakeSeatApp> kbd;
  kbd.OnEnter(0u, nullptr, nullptr);  // must not crash
}

TEST(KeyboardHandler, OnLeaveIsNoOp) {
  wl::KeyboardHandler<FakeSeatApp> kbd;
  kbd.OnLeave(0u, nullptr);  // must not crash
}

TEST(KeyboardHandler, OnRepeatInfoIsNoOp) {
  wl::KeyboardHandler<FakeSeatApp> kbd;
  kbd.OnRepeatInfo(25, 400);  // must not crash
}

TEST(KeyboardHandler, OnKeymapNonXkbV1ClosesFd) {
  // For format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 the keymap is ignored but
  // the fd must still be closed (protocol transfers ownership).
  int p[2];
  ASSERT_EQ(::pipe(p), 0);
  const int read_end = p[0];
  ::close(p[1]);  // write end not needed

  wl::KeyboardHandler<FakeSeatApp> kbd;
  kbd.OnKeymap(WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, read_end, 0u);

  // After OnKeymap the fd must be closed; fcntl returns -1 when fd is closed.
  EXPECT_EQ(::fcntl(read_end, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}

// ── SeatManager tests ─────────────────────────────────────────────────────────

TEST(SeatManager, DefaultState) {
  wl::SeatManager<FakeSeatApp> sm;
  // No crash on construction or destruction.
}

TEST(SeatManager, BindWithoutRecordReturnsTrue) {
  // When Record() has not been called (name_ == 0), Bind() treats the seat as
  // optional and returns true immediately.
  wl::SeatManager<FakeSeatApp> sm;
  wl::CRegistry registry;  // null
  FakeSeatApp app;
  EXPECT_TRUE(sm.Bind(registry, &app));
}

TEST(SeatManager, BindWithRecordAndNullRegistryReturnsFalse) {
  // When Record() has been called (seat was advertised) but the registry is
  // null, Bind() must fail gracefully and return false.
  wl::SeatManager<FakeSeatApp> sm;
  sm.Record(1u, 5u);
  wl::CRegistry registry;  // null
  FakeSeatApp app;
  EXPECT_FALSE(sm.Bind(registry, &app));
}

TEST(SeatManager, ReleaseOnEmptySeatIsNoOp) {
  wl::SeatManager<FakeSeatApp> sm;
  sm.Release();  // must not crash
}
