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

#include <wl/seat.hpp>  // also pulls in keyboard.hpp

#include <fcntl.h>     // F_GETFD, fcntl
#include <sys/mman.h>  // memfd_create, mmap, munmap
#include <unistd.h>    // pipe, ftruncate, close

#include <gtest/gtest.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>  // free
#include <cstring>  // strlen, memcpy

// ── Minimal App stubs
// ─────────────────────────────────────────────────────────

// Minimal App: implements only the required OnKey(const wl::KeyEvent&) sink.
struct FakeSeatApp {
  uint32_t last_key = 0;
  uint32_t last_state = 0;
  xkb_keysym_t last_sym = XKB_KEY_NoSymbol;
  bool last_repeat = false;

  void OnKey(const wl::KeyEvent& ev) {
    last_key = ev.key;
    last_state = ev.state;
    last_sym = ev.keysym;
    last_repeat = ev.repeat;
  }
};

// App that also implements the optional OnKeymap(xkb_keymap*) hook — exercises
// the SFINAE keymap dispatch.
struct FakeSeatAppWithKeySym {
  uint32_t last_key = 0;
  uint32_t last_state = 0;
  xkb_keysym_t last_sym = XKB_KEY_NoSymbol;
  xkb_keymap* last_keymap = nullptr;

  void OnKey(const wl::KeyEvent& ev) {
    last_key = ev.key;
    last_state = ev.state;
    last_sym = ev.keysym;
  }
  void OnKeymap(xkb_keymap* keymap) { last_keymap = keymap; }
};

// ── KeyboardHandler tests
// ─────────────────────────────────────────────────────

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
  // Same as above but with an App that also implements OnKeymap.
  // With no keymap, the xkb_state_ branch is skipped so the KeyEvent's keysym
  // stays XKB_KEY_NoSymbol.
  wl::KeyboardHandler<FakeSeatAppWithKeySym> kbd;
  FakeSeatAppWithKeySym app;
  kbd.app_ = &app;
  kbd.OnKey(0u, 0u, 30u, 1u);
  EXPECT_EQ(app.last_key, 30u);
  EXPECT_EQ(app.last_state, 1u);
  EXPECT_EQ(app.last_sym,
            xkb_keysym_t{XKB_KEY_NoSymbol});  // not set — no xkb_state
}

TEST(KeyboardHandler, ServerRepeatStateNormalizedToPressedRepeat) {
  // wl_keyboard v10: a key event with state "repeated" (2) — compositor-driven
  // repeat — must reach the App as a pressed KeyEvent flagged repeat=true, so
  // server-driven and client-driven repeat are handled through one path.
  wl::KeyboardHandler<FakeSeatApp> kbd;
  FakeSeatApp app;
  kbd.app_ = &app;
  constexpr uint32_t kStateRepeated = 2u;
  kbd.OnKey(0u, 0u, 30u, kStateRepeated);
  EXPECT_EQ(app.last_key, 30u);
  EXPECT_EQ(app.last_state, uint32_t{WL_KEYBOARD_KEY_STATE_PRESSED});
  EXPECT_TRUE(app.last_repeat);
}

TEST(KeyboardHandler, RealKeyIsNotFlaggedRepeat) {
  // A genuine press (state 1) must not be flagged as a repeat.
  wl::KeyboardHandler<FakeSeatApp> kbd;
  FakeSeatApp app;
  kbd.app_ = &app;
  kbd.OnKey(0u, 0u, 30u, WL_KEYBOARD_KEY_STATE_PRESSED);
  EXPECT_EQ(app.last_state, uint32_t{WL_KEYBOARD_KEY_STATE_PRESSED});
  EXPECT_FALSE(app.last_repeat);
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

TEST(KeyboardHandler, OnKeymapXkbV1CompilesStateAndFiresHook) {
  // Compile a real xkb keymap, hand it to OnKeymap over a memfd, and verify
  // both that the optional OnKeymap(xkb_keymap*) hook fires and that a
  // subsequent key resolves a real keysym through the compiled state.
  xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  ASSERT_NE(ctx, nullptr);
  xkb_keymap* km =
      xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
  ASSERT_NE(km, nullptr);
  char* km_str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
  ASSERT_NE(km_str, nullptr);
  const std::size_t size = std::strlen(km_str) + 1;  // include NUL terminator

  const int fd = ::memfd_create("test-keymap", MFD_CLOEXEC);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(size)), 0);
  void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ASSERT_NE(map, MAP_FAILED);
  std::memcpy(map, km_str, size);
  ::munmap(map, size);
  std::free(km_str);
  xkb_keymap_unref(km);
  xkb_context_unref(ctx);

  wl::KeyboardHandler<FakeSeatAppWithKeySym> kbd;
  FakeSeatAppWithKeySym app;
  kbd.app_ = &app;
  // Unbound handler (null proxy) → OnKeymap takes the MAP_PRIVATE default path.
  // OnKeymap closes fd.
  kbd.OnKeymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
               static_cast<uint32_t>(size));

  // The optional keymap hook fired with the compiled keymap.
  EXPECT_NE(app.last_keymap, nullptr);

  // With xkb_state compiled, a key now resolves a real keysym: evdev keycode
  // 30 (KEY_A) → XKB_KEY_a under the default US layout.
  kbd.OnKey(0u, 0u, 30u, WL_KEYBOARD_KEY_STATE_PRESSED);
  EXPECT_EQ(app.last_sym, xkb_keysym_t{XKB_KEY_a});
}

// ── SeatManager tests
// ─────────────────────────────────────────────────────────

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
