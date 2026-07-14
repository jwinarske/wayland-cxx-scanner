// SPDX-License-Identifier: MIT
// Tests for wl::AglShellHandler<App> (agl_shell.hpp).
//
// agl_shell_client.hpp is generated from agl-shell.xml at build time and
// must be included first so that CAglShell<> is visible when agl_shell.hpp
// is parsed.
//
// These tests exercise handler event delegation without a live Wayland
// compositor.  All request methods (_Marshal) are no-ops when the proxy
// is null.
#include "agl_shell_client.hpp"  // generated — must come before agl_shell.hpp

#include <wl/agl_shell.hpp>

#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>

// ── Minimal App stub
// ──────────────────────────────────────────────────────────

struct FakeAglApp {
  bool bound_ok = false;
  bool bound_fail = false;
  std::string last_app_id;
  uint32_t last_state = 0;

  void OnAglBoundOk() { bound_ok = true; }
  void OnAglBoundFail() { bound_fail = true; }
  void OnAglAppState(const char* app_id, uint32_t state) {
    last_app_id = app_id ? app_id : "";
    last_state = state;
  }
};

// App that also takes the optional version-8 app_on_output hook.
struct FakeAglAppOnOutput {
  std::string last_app_id;
  std::string last_output;

  void OnAglBoundOk() {}
  void OnAglBoundFail() {}
  void OnAglAppState(const char*, uint32_t) {}
  void OnAglAppOnOutput(const char* app_id, const char* output_name) {
    last_app_id = app_id ? app_id : "";
    last_output = output_name ? output_name : "";
  }
};

// ── AglShellHandler tests
// ─────────────────────────────────────────────────────

TEST(AglShellHandler, DefaultIsNull) {
  wl::AglShellHandler<FakeAglApp> h;
  EXPECT_TRUE(h.IsNull());
  EXPECT_EQ(h.app_, nullptr);
}

TEST(AglShellHandler, OnBoundOkWithNullAppIsNoOp) {
  // When app_ is null, events must be silently dropped.
  wl::AglShellHandler<FakeAglApp> h;
  h.OnBoundOk();  // must not crash
}

TEST(AglShellHandler, OnBoundFailWithNullAppIsNoOp) {
  wl::AglShellHandler<FakeAglApp> h;
  h.OnBoundFail();  // must not crash
}

TEST(AglShellHandler, OnAppStateWithNullAppIsNoOp) {
  wl::AglShellHandler<FakeAglApp> h;
  h.OnAppState("my-app", 1u);  // must not crash
}

TEST(AglShellHandler, OnBoundOkDelegatesToApp) {
  wl::AglShellHandler<FakeAglApp> h;
  FakeAglApp app;
  h.app_ = &app;
  h.OnBoundOk();
  EXPECT_TRUE(app.bound_ok);
  EXPECT_FALSE(app.bound_fail);
}

TEST(AglShellHandler, OnBoundFailDelegatesToApp) {
  wl::AglShellHandler<FakeAglApp> h;
  FakeAglApp app;
  h.app_ = &app;
  h.OnBoundFail();
  EXPECT_TRUE(app.bound_fail);
  EXPECT_FALSE(app.bound_ok);
}

TEST(AglShellHandler, OnAppStateDelegatesToApp) {
  wl::AglShellHandler<FakeAglApp> h;
  FakeAglApp app;
  h.app_ = &app;
  h.OnAppState("com.example.app", 2u);
  EXPECT_EQ(app.last_app_id, "com.example.app");
  EXPECT_EQ(app.last_state, 2u);
}

// ── app_on_output (version 8) ────────────────────────────────────────────────

TEST(AglShellHandler, OnAppOnOutputDelegatesWhenTheAppTakesIt) {
  wl::AglShellHandler<FakeAglAppOnOutput> h;
  FakeAglAppOnOutput app;
  h.app_ = &app;
  h.OnAppOnOutput("com.example.app", "HDMI-A-1");
  EXPECT_EQ(app.last_app_id, "com.example.app");
  EXPECT_EQ(app.last_output, "HDMI-A-1");
}

TEST(AglShellHandler, OnAppOnOutputIsSwallowedWhenTheAppOmitsIt) {
  // FakeAglApp predates version 8 and defines no OnAglAppOnOutput.  It must
  // still compile and must not crash — a shell written against an older
  // agl_shell cannot be forced to grow a hook to keep building.
  wl::AglShellHandler<FakeAglApp> h;
  FakeAglApp app;
  h.app_ = &app;
  h.OnAppOnOutput("com.example.app", "HDMI-A-1");
  SUCCEED();
}

TEST(AglShellHandler, OnAppOnOutputWithNullAppIsNoOp) {
  wl::AglShellHandler<FakeAglAppOnOutput> h;  // app_ left null
  h.OnAppOnOutput("com.example.app", "HDMI-A-1");
  SUCCEED();
}
