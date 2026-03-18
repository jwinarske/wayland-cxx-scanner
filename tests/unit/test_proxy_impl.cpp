// SPDX-License-Identifier: MIT
// Tests for wl::CProxyImpl<Derived, Traits> and wl::construct<>.
// These tests exercise null-path methods without a live Wayland compositor.
#include <wl/proxy_impl.hpp>

#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>

// ── Minimal fake traits
// ───────────────────────────────────────────────────────

struct FakeClientTraits {
  static constexpr std::string_view interface_name = "wl_fake_client";
  static constexpr uint32_t version = 1;
  static const wl_interface& wl_iface() noexcept {
    static wl_interface s{};
    return s;
  }
};
static_assert(wl::WlProxyTraits<FakeClientTraits>);

struct FakeChildTraits {
  static constexpr std::string_view interface_name = "wl_fake_child";
  static constexpr uint32_t version = 2;
  static const wl_interface& wl_iface() noexcept {
    static wl_interface s{};
    return s;
  }
};
static_assert(wl::WlProxyTraits<FakeChildTraits>);

// ── Concrete CProxyImpl-derived class
// ─────────────────────────────────────────

struct FakeClient : wl::CProxyImpl<FakeClient, FakeClientTraits> {
  // Required by _SetProxy: wl_proxy_add_listener reads s_listener_table_.
  // An array of one null pointer is sufficient for null-path tests.
  static const void* s_listener_table_[];

  // CEventMap::ProcessEvent is pure virtual; provide a no-op implementation.
  bool ProcessEvent(uint32_t /*opcode*/, void** /*args*/) override {
    return false;
  }
};
const void* FakeClient::s_listener_table_[] = {nullptr};

// ── CProxyImpl tests
// ──────────────────────────────────────────────────────────

TEST(CProxyImpl, DefaultIsNull) {
  FakeClient c;
  EXPECT_TRUE(c.IsNull());
}

TEST(CProxyImpl, BoolConversionFalseWhenNull) {
  FakeClient c;
  EXPECT_FALSE(static_cast<bool>(c));
}

TEST(CProxyImpl, SetProxyNullIsSafe) {
  // _SetProxy(nullptr) must not install any listener; proxy stays null.
  FakeClient c;
  c._SetProxy(nullptr);
  EXPECT_TRUE(c.IsNull());
}

TEST(CProxyImpl, MarshalNullProxyIsNoOp) {
  // _Marshal on a null proxy must not crash (guarded by internal null check).
  FakeClient c;
  c._Marshal(0u);  // no-op
}

TEST(CProxyImpl, MarshalNewNullProxyReturnsNullptr) {
  // _MarshalNew on a null proxy must return nullptr (guarded by internal
  // check).
  FakeClient c;
  wl_proxy* result = c._MarshalNew(0u, nullptr);
  EXPECT_EQ(result, nullptr);
}

// ── wl::construct<> tests
// ─────────────────────────────────────────────────────

TEST(Construct, NullParentReturnsNullptr) {
  // construct<> delegates to _MarshalNew; null parent must return nullptr.
  FakeClient parent;  // null proxy
  wl_proxy* result = wl::construct<FakeChildTraits, 0u>(parent);
  EXPECT_EQ(result, nullptr);
}
