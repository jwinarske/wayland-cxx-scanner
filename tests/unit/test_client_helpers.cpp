// SPDX-License-Identifier: MIT
// Tests for wl::SetupHandler<> and wl::BindHandler<>.
// These tests exercise null-path logic without a live Wayland compositor.
#include <wl/client_helpers.hpp>

#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>

// ── Minimal fake traits and CProxyImpl-derived handler ───────────────────────

struct FakeHelperTraits {
  static constexpr std::string_view interface_name = "wl_fake_helper";
  static constexpr uint32_t version = 1;
  static const wl_interface& wl_iface() noexcept {
    static wl_interface s{};
    return s;
  }
};
static_assert(wl::WlProxyTraits<FakeHelperTraits>);

struct FakeHandler : wl::CProxyImpl<FakeHandler, FakeHelperTraits> {
  // Required by _SetProxy when proxy is non-null; a null-entry table is
  // sufficient for these null-path tests.
  static const void* s_listener_table_[];

  // CEventMap::ProcessEvent is pure virtual; provide a no-op implementation.
  bool ProcessEvent(uint32_t /*opcode*/, void** /*args*/) override {
    return false;
  }
};
const void* FakeHandler::s_listener_table_[] = {nullptr};

// ── SetupHandler tests
// ────────────────────────────────────────────────────────

TEST(SetupHandler, NullRawReturnsFalse) {
  wl::WlPtr<FakeHandler> ptr;
  EXPECT_FALSE(wl::SetupHandler(ptr, nullptr));
}

TEST(SetupHandler, PtrRemainsNullWhenRawIsNull) {
  // Calling SetupHandler with null raw must leave the WlPtr unchanged.
  wl::WlPtr<FakeHandler> ptr;
  EXPECT_FALSE(wl::SetupHandler(ptr, nullptr));
  EXPECT_TRUE(ptr.IsNull());
}

// ── BindHandler tests
// ─────────────────────────────────────────────────────────

TEST(BindHandler, NullRegistryReturnsFalse) {
  // A null CRegistry returns nullptr from Bind<>, which causes SetupHandler
  // (called internally) to return false.
  wl::CRegistry registry;  // null — not connected to a display
  wl::WlPtr<FakeHandler> ptr;
  EXPECT_FALSE(wl::BindHandler<FakeHelperTraits>(registry, ptr, 1u, 1u));
}

TEST(BindHandler, PtrRemainsNullWhenRegistryIsNull) {
  wl::CRegistry registry;
  wl::WlPtr<FakeHandler> ptr;
  EXPECT_FALSE(wl::BindHandler<FakeHelperTraits>(registry, ptr, 1u, 1u));
  EXPECT_TRUE(ptr.IsNull());
}
