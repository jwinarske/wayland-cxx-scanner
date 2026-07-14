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

#include <fcntl.h>                    // F_GETFD, fcntl
#include <linux/input-event-codes.h>  // BTN_LEFT
#include <sys/mman.h>                 // memfd_create, mmap, munmap
#include <unistd.h>                   // pipe, ftruncate, close
#include <wayland-client-protocol.h>  // WL_POINTER_BUTTON_STATE_PRESSED

#include <gtest/gtest.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>  // free
#include <cstring>  // strlen, memcpy
#include <vector>

namespace {
// wl_fixed_t is 24.8 fixed point.
wl_fixed_t Fixed(double v) {
  return static_cast<wl_fixed_t>(v * 256.0);
}
}  // namespace

// ── Minimal App stubs
// ─────────────────────────────────────────────────────────

// App with a pointer button hook; also tracks motion + enter to check the
// position the handler carries into button events.
struct FakePointerApp {
  wl::PointerButtonEvent last_button{};
  int button_count = 0;
  wl::PointerEvent last_motion{};
  bool left = false;

  void OnPointerEnter(const wl::PointerEvent& ev) { last_motion = ev; }
  void OnPointerMotion(const wl::PointerEvent& ev) { last_motion = ev; }
  void OnPointerButton(const wl::PointerButtonEvent& ev) {
    last_button = ev;
    ++button_count;
  }
  void OnPointerLeave() { left = true; }
};

// App with only the scroll hooks — also proves an axis-only consumer still gets
// a pointer bound (wl::detail::WantsPointer).
struct FakeScrollApp {
  std::vector<wl::PointerAxisEvent> axes;
  int frames = 0;

  void OnPointerAxis(const wl::PointerAxisEvent& ev) { axes.push_back(ev); }
  void OnPointerFrame() { ++frames; }
};

// App that wants only the frame boundary — e.g. to coalesce one redraw per
// pointer batch.  Must still get a pointer bound.
struct FakeFrameOnlyApp {
  int frames = 0;

  void OnPointerFrame() { ++frames; }
};

// App with the full set of touch hooks.
struct FakeTouchApp {
  int downs = 0;
  int ups = 0;
  int motions = 0;
  int cancels = 0;
  wl::TouchPoint last_down{};
  std::int32_t last_up = -99;
  std::vector<wl::TouchPoint> last_frame;

  void OnTouchDown(const wl::TouchPoint& p) {
    last_down = p;
    ++downs;
  }
  void OnTouchMotion(const wl::TouchPoint&) { ++motions; }
  void OnTouchUp(std::int32_t id) {
    last_up = id;
    ++ups;
  }
  void OnTouchFrame(wl::span<const wl::TouchPoint> pts) {
    last_frame.assign(pts.begin(), pts.end());
  }
  void OnTouchCancel() { ++cancels; }
};

// Minimal App: implements only the required OnKey(const wl::KeyEvent&) sink.
struct FakeSeatApp {
  uint32_t last_key = 0;
  uint32_t last_state = 0;
  xkb_keysym_t last_sym = XKB_KEY_NoSymbol;
  bool last_repeat = false;
  uint32_t last_serial = 0;

  void OnKey(const wl::KeyEvent& ev) {
    last_key = ev.key;
    last_state = ev.state;
    last_sym = ev.keysym;
    last_repeat = ev.repeat;
    last_serial = ev.serial;
  }
};

// App that also implements the optional OnKeymap(xkb_keymap*) hook — exercises
// the SFINAE keymap dispatch.
struct FakeSeatAppWithKeySym {
  uint32_t last_key = 0;
  uint32_t last_state = 0;
  xkb_keysym_t last_sym = XKB_KEY_NoSymbol;
  xkb_keymap* last_keymap = nullptr;
  bool keyboard_left = false;

  void OnKey(const wl::KeyEvent& ev) {
    last_key = ev.key;
    last_state = ev.state;
    last_sym = ev.keysym;
  }
  void OnKeymap(xkb_keymap* keymap) { last_keymap = keymap; }
  void OnKeyboardLeave() { keyboard_left = true; }
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

TEST(KeyboardHandler, OnKeyDeliversTheInputSerial) {
  // The wl_keyboard.key serial reaches the App on the KeyEvent — consumers
  // need it for requests that take an input serial (e.g. set_selection).
  wl::KeyboardHandler<FakeSeatApp> kbd;
  FakeSeatApp app;
  kbd.app_ = &app;
  kbd.OnKey(4242u, 0u, 30u, 1u);
  EXPECT_EQ(app.last_serial, 4242u);
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
  kbd.OnLeave(0u, nullptr);  // must not crash (App has no OnKeyboardLeave)
}

TEST(KeyboardHandler, OnLeaveFiresOptionalKeyboardLeaveHook) {
  // When the App provides OnKeyboardLeave(), the wl_keyboard.leave event must
  // invoke it via the SFINAE hook. The name is distinct from the IME
  // wl::ime::TextInputListener::OnLeave() so the two never collide.
  wl::KeyboardHandler<FakeSeatAppWithKeySym> kbd;
  FakeSeatAppWithKeySym app;
  kbd.app_ = &app;
  EXPECT_FALSE(app.keyboard_left);
  kbd.OnLeave(0u, nullptr);
  EXPECT_TRUE(app.keyboard_left);
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

// ── PointerHandler tests ─────────────────────────────────────────────────────

TEST(PointerHandler, ButtonCarriesLastMotionPosition) {
  wl::PointerHandler<FakePointerApp> ptr;
  FakePointerApp app;
  ptr.app_ = &app;

  // 12.5, 34.25 in wl_fixed (24.8): *256.
  ptr.OnMotion(0u, static_cast<wl_fixed_t>(12.5 * 256),
               static_cast<wl_fixed_t>(34.25 * 256));
  EXPECT_DOUBLE_EQ(app.last_motion.x, 12.5);
  EXPECT_DOUBLE_EQ(app.last_motion.y, 34.25);

  ptr.OnButton(7u, 100u, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
  EXPECT_EQ(app.button_count, 1);
  EXPECT_DOUBLE_EQ(app.last_button.x, 12.5);
  EXPECT_DOUBLE_EQ(app.last_button.y, 34.25);
  EXPECT_EQ(app.last_button.serial, 7u);
  EXPECT_EQ(app.last_button.button, static_cast<uint32_t>(BTN_LEFT));
  EXPECT_EQ(app.last_button.state,
            static_cast<uint32_t>(WL_POINTER_BUTTON_STATE_PRESSED));
}

TEST(PointerHandler, EnterSeedsPositionAndLeaveFires) {
  wl::PointerHandler<FakePointerApp> ptr;
  FakePointerApp app;
  ptr.app_ = &app;

  ptr.OnEnter(3u, nullptr, static_cast<wl_fixed_t>(5 * 256),
              static_cast<wl_fixed_t>(6 * 256));
  ptr.OnButton(3u, 0u, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
  EXPECT_DOUBLE_EQ(app.last_button.x, 5.0);
  EXPECT_DOUBLE_EQ(app.last_button.y, 6.0);

  EXPECT_FALSE(app.left);
  ptr.OnLeave(4u, nullptr);
  EXPECT_TRUE(app.left);
}

TEST(PointerHandler, NullAppIsNoOp) {
  wl::PointerHandler<FakePointerApp> ptr;  // app_ left null
  ptr.OnMotion(0u, 0, 0);
  ptr.OnButton(0u, 0u, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);  // no crash
}

TEST(PointerHandler, WantsPointerDetectsHooks) {
  static_assert(wl::detail::WantsPointer<FakePointerApp>,
                "App with pointer hooks should be detected");
  static_assert(wl::detail::WantsPointer<FakeScrollApp>,
                "App with only a scroll hook still needs a pointer");
  static_assert(wl::detail::WantsPointer<FakeFrameOnlyApp>,
                "App with only a frame hook still needs a pointer");
  static_assert(!wl::detail::WantsPointer<FakeSeatApp>,
                "keyboard-only App must not request a pointer");
  SUCCEED();
}

// ── PointerHandler axis (scroll) tests ───────────────────────────────────────
//
// Each test pins one row of the version/source normalization table in
// wl/pointer.hpp.  version_ stands in for the negotiated seat version, which a
// live SeatManager sets from the seat proxy.

namespace {
// Builds a handler + app pair at a given negotiated wl_pointer version.
struct ScrollFixture {
  wl::PointerHandler<FakeScrollApp> ptr;
  FakeScrollApp app;

  explicit ScrollFixture(uint32_t version) {
    ptr.app_ = &app;
    ptr.version_ = version;
  }
};
}  // namespace

TEST(PointerHandlerAxis, Value120PathOnSeat8) {
  ScrollFixture f(8u);

  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(50u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
  EXPECT_TRUE(f.app.axes.empty()) << "axis must not be delivered before frame";

  f.ptr.OnFrame();
  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_EQ(f.app.axes[0].axis,
            static_cast<uint32_t>(WL_POINTER_AXIS_VERTICAL_SCROLL));
  EXPECT_EQ(f.app.axes[0].value120, 120);
  EXPECT_DOUBLE_EQ(f.app.axes[0].continuous, 10.0);
  EXPECT_EQ(f.app.axes[0].source,
            static_cast<uint32_t>(WL_POINTER_AXIS_SOURCE_WHEEL));
  EXPECT_EQ(f.app.axes[0].time, 50u);
  EXPECT_FALSE(f.app.axes[0].stop);
  EXPECT_EQ(f.app.frames, 1);
}

TEST(PointerHandlerAxis, Value120WinsOverDiscreteOnSeat8) {
  ScrollFixture f(8u);

  // A v8 compositor sends both for compatibility; a half-notch high-resolution
  // wheel reports value120 = 60 while axis_discrete rounds to 0.
  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(1u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(5.0));
  f.ptr.OnAxisDiscrete(WL_POINTER_AXIS_VERTICAL_SCROLL, 0);
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 60);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_EQ(f.app.axes[0].value120, 60) << "axis_discrete must be ignored";
}

TEST(PointerHandlerAxis, DiscretePathOnSeat5) {
  ScrollFixture f(5u);

  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(20u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(-10.0));
  f.ptr.OnAxisDiscrete(WL_POINTER_AXIS_VERTICAL_SCROLL, -1);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_EQ(f.app.axes[0].value120, -120) << "one notch up == -120";
  EXPECT_DOUBLE_EQ(f.app.axes[0].continuous, -10.0);
  EXPECT_EQ(f.app.frames, 1);
}

TEST(PointerHandlerAxis, ContinuousSourceSynthesizesValue120) {
  ScrollFixture f(8u);

  // A finger swipe sends neither value120 nor discrete — 10 units per notch.
  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
  f.ptr.OnAxis(30u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(5.0));
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_EQ(f.app.axes[0].value120, 60) << "half a notch";
  EXPECT_DOUBLE_EQ(f.app.axes[0].continuous, 5.0)
      << "raw distance must pass through for smooth scrolling";
  EXPECT_EQ(f.app.axes[0].source,
            static_cast<uint32_t>(WL_POINTER_AXIS_SOURCE_FINGER));
}

TEST(PointerHandlerAxis, PreV5FlushesPerEventWithoutFrame) {
  ScrollFixture f(4u);

  // Version 4 has no frame, no source, no discrete: each axis stands alone.
  f.ptr.OnAxis(10u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  ASSERT_EQ(f.app.axes.size(), 1u) << "pre-v5 must not wait for a frame";
  EXPECT_EQ(f.app.axes[0].value120, 120);
  EXPECT_EQ(f.app.axes[0].source, wl::kAxisSourceUnknown);
  EXPECT_EQ(f.app.frames, 1) << "each pre-v5 event is its own implicit frame";

  f.ptr.OnAxis(11u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  ASSERT_EQ(f.app.axes.size(), 2u);
  EXPECT_EQ(f.app.axes[1].value120, 120) << "state must not carry over";
}

TEST(PointerHandlerAxis, MixedHorizontalAndVerticalInOneFrame) {
  ScrollFixture f(8u);

  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(60u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
  f.ptr.OnAxis(60u, WL_POINTER_AXIS_HORIZONTAL_SCROLL, Fixed(-10.0));
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_HORIZONTAL_SCROLL, -120);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 2u) << "one event per scrolled axis";
  EXPECT_EQ(f.app.axes[0].axis,
            static_cast<uint32_t>(WL_POINTER_AXIS_VERTICAL_SCROLL));
  EXPECT_EQ(f.app.axes[0].value120, 120);
  EXPECT_EQ(f.app.axes[1].axis,
            static_cast<uint32_t>(WL_POINTER_AXIS_HORIZONTAL_SCROLL));
  EXPECT_EQ(f.app.axes[1].value120, -120);
  EXPECT_EQ(f.app.frames, 1) << "one frame regardless of axis count";
}

TEST(PointerHandlerAxis, AccumulatesRepeatedEventsWithinAFrame) {
  ScrollFixture f(8u);

  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(70u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
  f.ptr.OnAxis(71u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u) << "one event per axis per frame";
  EXPECT_EQ(f.app.axes[0].value120, 240);
  EXPECT_DOUBLE_EQ(f.app.axes[0].continuous, 20.0);
  EXPECT_EQ(f.app.axes[0].time, 71u) << "latest timestamp wins";
}

TEST(PointerHandlerAxis, StopDeliversZeroValueEvent) {
  ScrollFixture f(8u);

  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
  f.ptr.OnAxisStop(80u, WL_POINTER_AXIS_VERTICAL_SCROLL);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_TRUE(f.app.axes[0].stop);
  EXPECT_EQ(f.app.axes[0].value120, 0) << "a lone stop accumulated no distance";
  EXPECT_DOUBLE_EQ(f.app.axes[0].continuous, 0.0);
  EXPECT_EQ(f.app.axes[0].time, 80u);
}

TEST(PointerHandlerAxis, StopInTheSameFrameKeepsTheFinalDistance) {
  ScrollFixture f(8u);

  // A finger lifting off can report its last movement and the stop together;
  // value120 and continuous must agree rather than contradict each other.
  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
  f.ptr.OnAxis(85u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(5.0));
  f.ptr.OnAxisStop(85u, WL_POINTER_AXIS_VERTICAL_SCROLL);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_TRUE(f.app.axes[0].stop);
  EXPECT_EQ(f.app.axes[0].value120, 60) << "the last 5.0 must not be dropped";
  EXPECT_DOUBLE_EQ(f.app.axes[0].continuous, 5.0);
}

TEST(PointerHandlerAxis, HostileAccumulationSaturatesInsteadOfWrapping) {
  ScrollFixture f(8u);

  // A compositor is free to pack any number of events into one frame; summing
  // them must not overflow into a scroll in the opposite direction.
  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  for (int i = 0; i < 3; ++i) {
    f.ptr.OnAxis(120u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(1.0));
    f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, INT32_MAX);
  }
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_EQ(f.app.axes[0].value120, INT32_MAX) << "saturate, never wrap";
}

TEST(PointerHandlerAxis, HostileDiscreteSaturatesInsteadOfWrapping) {
  ScrollFixture f(5u);

  // discrete * 120 overflows int32 for |discrete| > ~17.9M.
  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(130u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(1.0));
  f.ptr.OnAxisDiscrete(WL_POINTER_AXIS_VERTICAL_SCROLL, 20000000);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_EQ(f.app.axes[0].value120, INT32_MAX);
}

TEST(PointerHandlerAxis, HostileContinuousDistanceSaturates) {
  ScrollFixture f(8u);

  // wl_fixed_t maxes near 8.4e6 per event; enough of them in one frame would
  // push distance * 12 past int32 and make the narrowing conversion undefined.
  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_FINGER);
  for (int i = 0; i < 40; ++i)
    f.ptr.OnAxis(140u, WL_POINTER_AXIS_VERTICAL_SCROLL, INT32_MAX);
  f.ptr.OnFrame();

  ASSERT_EQ(f.app.axes.size(), 1u);
  EXPECT_EQ(f.app.axes[0].value120, INT32_MAX);
}

TEST(PointerHandlerAxis, FrameResetsAccumulator) {
  ScrollFixture f(8u);

  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(90u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
  f.ptr.OnFrame();
  f.ptr.OnFrame();  // empty frame

  EXPECT_EQ(f.app.axes.size(), 1u) << "a second frame must not re-deliver";
  EXPECT_EQ(f.app.frames, 2);
}

TEST(PointerHandlerAxis, LeaveDropsPendingAccumulation) {
  ScrollFixture f(8u);

  f.ptr.OnAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
  f.ptr.OnAxis(100u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  f.ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
  f.ptr.OnLeave(5u, nullptr);
  f.ptr.OnFrame();

  EXPECT_TRUE(f.app.axes.empty())
      << "scroll accumulated before leave must not land on the next surface";
}

TEST(PointerHandlerAxis, OutOfRangeAxisIsIgnored) {
  ScrollFixture f(8u);

  f.ptr.OnAxis(110u, 99u, Fixed(10.0));  // no such axis
  f.ptr.OnAxisValue120(99u, 120);
  f.ptr.OnFrame();

  EXPECT_TRUE(f.app.axes.empty());
}

TEST(PointerHandlerAxis, NullAppIsNoOp) {
  wl::PointerHandler<FakeScrollApp> ptr;  // app_ left null
  ptr.version_ = 8u;
  ptr.OnAxis(0u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  ptr.OnAxisValue120(WL_POINTER_AXIS_VERTICAL_SCROLL, 120);
  ptr.OnFrame();  // no crash
}

TEST(PointerHandlerAxis, AppWithoutScrollHooksCompiles) {
  // FakePointerApp defines no axis hook; the handler must swallow the events.
  wl::PointerHandler<FakePointerApp> ptr;
  FakePointerApp app;
  ptr.app_ = &app;
  ptr.version_ = 8u;
  ptr.OnAxis(0u, WL_POINTER_AXIS_VERTICAL_SCROLL, Fixed(10.0));
  ptr.OnFrame();
  SUCCEED();
}

// ── TouchHandler tests ───────────────────────────────────────────────────────

TEST(TouchHandler, TracksTenConcurrentContacts) {
  wl::TouchHandler<FakeTouchApp> t;
  FakeTouchApp app;
  t.app_ = &app;

  for (std::int32_t i = 0; i < 10; ++i)
    t.OnDown(1u, 0u, nullptr, i, Fixed(i), Fixed(i * 2));

  EXPECT_EQ(app.downs, 10);
  EXPECT_EQ(t.active().size(), 10u);

  t.OnFrame();
  ASSERT_EQ(app.last_frame.size(), 10u);
  EXPECT_EQ(app.last_frame[3].id, 3);
  EXPECT_DOUBLE_EQ(app.last_frame[3].x, 3.0);
  EXPECT_DOUBLE_EQ(app.last_frame[3].y, 6.0);
}

TEST(TouchHandler, MotionUpdatesAndUpCompacts) {
  wl::TouchHandler<FakeTouchApp> t;
  FakeTouchApp app;
  t.app_ = &app;

  t.OnDown(1u, 0u, nullptr, 5, Fixed(1), Fixed(1));
  t.OnDown(1u, 0u, nullptr, 7, Fixed(2), Fixed(2));
  t.OnMotion(0u, 5, Fixed(9), Fixed(9));
  EXPECT_EQ(app.motions, 1);

  double x5 = -1.0;
  for (const wl::TouchPoint& p : t.active()) {
    if (p.id == 5)
      x5 = p.x;
  }
  EXPECT_DOUBLE_EQ(x5, 9.0);

  // Lifting id 5 swap-removes it, leaving id 7 as the sole compact entry.
  t.OnUp(1u, 0u, 5);
  EXPECT_EQ(app.last_up, 5);
  ASSERT_EQ(t.active().size(), 1u);
  EXPECT_EQ(t.active()[0].id, 7);
}

TEST(TouchHandler, OverCapacityContactIsDropped) {
  wl::TouchHandler<FakeTouchApp> t;
  FakeTouchApp app;
  t.app_ = &app;

  for (std::int32_t i = 0; i < 12; ++i)
    t.OnDown(1u, 0u, nullptr, i, Fixed(i), Fixed(i));

  EXPECT_EQ(t.active().size(), wl::TouchHandler<FakeTouchApp>::kMaxPoints);
  EXPECT_EQ(app.downs, 10);  // only kMaxPoints accepted; extras ignored
}

TEST(TouchHandler, CancelClearsAll) {
  wl::TouchHandler<FakeTouchApp> t;
  FakeTouchApp app;
  t.app_ = &app;

  t.OnDown(1u, 0u, nullptr, 1, Fixed(1), Fixed(1));
  t.OnDown(1u, 0u, nullptr, 2, Fixed(2), Fixed(2));
  t.OnCancel();
  EXPECT_EQ(app.cancels, 1);
  EXPECT_TRUE(t.active().empty());
}

TEST(TouchHandler, NullAppTracksWithoutCrash) {
  wl::TouchHandler<FakeTouchApp> t;  // app_ left null
  t.OnDown(1u, 0u, nullptr, 0, Fixed(4), Fixed(4));
  EXPECT_EQ(t.active().size(), 1u);
  t.OnMotion(0u, 0, Fixed(5), Fixed(5));
  t.OnUp(1u, 0u, 0);
  t.OnFrame();
  t.OnCancel();
  EXPECT_TRUE(t.active().empty());
}

TEST(TouchHandler, WantsTouchDetectsHooks) {
  static_assert(wl::detail::WantsTouch<FakeTouchApp>,
                "App with touch hooks should be detected");
  static_assert(!wl::detail::WantsTouch<FakeSeatApp>,
                "keyboard-only App must not request touch");
  static_assert(!wl::detail::WantsTouch<FakePointerApp>,
                "pointer-only App must not request touch");
  SUCCEED();
}
