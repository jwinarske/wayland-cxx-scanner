// SPDX-License-Identifier: MIT
// Tests for wl::XdgWmBaseHandler, wl::XdgSurfaceHandler<App>, and
// wl::XdgToplevelHandler<App> (xdg_shell.hpp).
//
// xdg_shell_client.hpp is generated from xdg-shell.xml at build time and
// must be included first so that CXdgWmBase<>, CXdgSurface<>, and
// CXdgToplevel<> are visible when xdg_shell.hpp is parsed.
//
// These tests exercise handler logic without a live Wayland compositor.
// All request methods (_Marshal) are no-ops when the proxy is null.
#include "xdg_shell_client.hpp"  // generated — must come before xdg_shell.hpp
#include <wl/xdg_shell.hpp>

#include <gtest/gtest.h>
#include <cstdint>

// ── Minimal App stubs ─────────────────────────────────────────────────────────

struct FakeXdgApp {
  uint32_t last_surface_serial = 0;
  int32_t last_w = -1;
  int32_t last_h = -1;
  bool closed = false;

  void OnXdgSurfaceConfigure(uint32_t serial) { last_surface_serial = serial; }
  void OnToplevelConfigure(int32_t w, int32_t h) {
    last_w = w;
    last_h = h;
  }
  void OnToplevelClose() { closed = true; }
};

// ── XdgWmBaseHandler tests ────────────────────────────────────────────────────

TEST(XdgWmBaseHandler, DefaultIsNull) {
  wl::XdgWmBaseHandler h;
  EXPECT_TRUE(h.IsNull());
}

TEST(XdgWmBaseHandler, OnPingDoesNotCrashWithNullProxy) {
  // OnPing calls Pong(serial) which calls _Marshal (no-op when proxy is null).
  wl::XdgWmBaseHandler h;
  h.OnPing(42u);  // must not crash
}

// ── XdgSurfaceHandler tests ───────────────────────────────────────────────────

TEST(XdgSurfaceHandler, DefaultIsNull) {
  wl::XdgSurfaceHandler<FakeXdgApp> h;
  EXPECT_TRUE(h.IsNull());
  EXPECT_EQ(h.app_, nullptr);
}

TEST(XdgSurfaceHandler, OnConfigureCallsAppAndAcks) {
  // AckConfigure is a no-op when proxy is null; OnXdgSurfaceConfigure must
  // still be called on the app.
  wl::XdgSurfaceHandler<FakeXdgApp> h;
  FakeXdgApp app;
  h.app_ = &app;
  h.OnConfigure(99u);
  EXPECT_EQ(app.last_surface_serial, 99u);
}

// ── XdgToplevelHandler tests ──────────────────────────────────────────────────

TEST(XdgToplevelHandler, DefaultIsNull) {
  wl::XdgToplevelHandler<FakeXdgApp> h;
  EXPECT_TRUE(h.IsNull());
  EXPECT_EQ(h.app_, nullptr);
}

TEST(XdgToplevelHandler, OnConfigureCallsApp) {
  wl::XdgToplevelHandler<FakeXdgApp> h;
  FakeXdgApp app;
  h.app_ = &app;
  h.OnConfigure(1280, 720, nullptr);
  EXPECT_EQ(app.last_w, 1280);
  EXPECT_EQ(app.last_h, 720);
}

TEST(XdgToplevelHandler, OnCloseCallsApp) {
  wl::XdgToplevelHandler<FakeXdgApp> h;
  FakeXdgApp app;
  h.app_ = &app;
  h.OnClose();
  EXPECT_TRUE(app.closed);
}

TEST(XdgToplevelHandler, OnConfigureBoundsIsNoOp) {
  wl::XdgToplevelHandler<FakeXdgApp> h;
  FakeXdgApp app;
  h.app_ = &app;
  // No crash and no side effect expected.
  h.OnConfigureBounds(800, 600);
}

TEST(XdgToplevelHandler, OnWmCapabilitiesIsNoOp) {
  wl::XdgToplevelHandler<FakeXdgApp> h;
  FakeXdgApp app;
  h.app_ = &app;
  // No crash and no side effect expected.
  h.OnWmCapabilities(nullptr);
}
