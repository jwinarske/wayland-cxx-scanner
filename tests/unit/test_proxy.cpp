// SPDX-License-Identifier: MIT
// Tests for wl::CProxy<Traits>, wl::WlPtr<T>, and wl::CRegistry.
// These tests do NOT require a live Wayland compositor: they exercise the
// handle-management and callback-storage logic with fake/mock pointers.
#include <wl/proxy.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

#include <gtest/gtest.h>
#include <string_view>

// ── Minimal fake traits (no real wl_interface needed for handle tests)
// ────────

struct FakeInterface {
  static constexpr std::string_view interface_name = "wl_fake";
  static constexpr uint32_t version = 3;
  static const wl_interface& wl_iface() noexcept {
    static wl_interface s{};
    return s;
  }
};

static_assert(wl::WlProxyTraits<FakeInterface>);
using FakeProxy = wl::CProxy<FakeInterface>;

// ── CProxy tests
// ──────────────────────────────────────────────────────────────

TEST(CProxy, DefaultIsNull) {
  FakeProxy p;
  EXPECT_TRUE(p.IsNull());
}
TEST(CProxy, AttachSets) {
  FakeProxy p;
  auto* f = reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0xDEAD));
  p.Attach(f);
  EXPECT_EQ(p.GetProxy(), f);
  p.Detach();  // avoid ~CProxy trying to destroy a fake pointer
}
TEST(CProxy, DetachClears) {
  FakeProxy p;
  auto* f = reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0xBEEF));
  p.Attach(f);
  EXPECT_EQ(p.Detach(), f);
  EXPECT_TRUE(p.IsNull());
}
TEST(CProxy, StaticMeta) {
  EXPECT_EQ(FakeProxy::InterfaceName(), "wl_fake");
  EXPECT_EQ(FakeProxy::MaxVersion(), 3u);
}
TEST(CProxy, BoolConversion) {
  FakeProxy p;
  EXPECT_FALSE(static_cast<bool>(p));
  auto* f = reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0x1));
  p.Attach(f);
  EXPECT_TRUE(static_cast<bool>(p));
  p.Detach();
}

// ── WlPtr tests
// ───────────────────────────────────────────────────────────────

struct FakeProxyObj : wl::CProxy<FakeInterface> {
  int destroy_count = 0;
  // Destroy without calling wl_proxy_destroy (fake pointer).
  void Destroy() noexcept {
    destroy_count++;
    Detach();
  }
};

TEST(WlPtr, DefaultIsNull) {
  wl::WlPtr<FakeProxyObj> p;
  EXPECT_TRUE(p.IsNull());
}
TEST(WlPtr, AttachAccess) {
  wl::WlPtr<FakeProxyObj> p;
  auto* f = reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0x1234));
  p.Attach(f);
  EXPECT_FALSE(p.IsNull());
  p->Detach();  // release without Destroy
}
TEST(WlPtr, MoveTransfers) {
  wl::WlPtr<FakeProxyObj> a;
  a.Attach(reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0xABCD)));
  wl::WlPtr<FakeProxyObj> b{std::move(a)};
  EXPECT_TRUE(a.IsNull());
  EXPECT_FALSE(b.IsNull());
  b->Detach();
}
TEST(WlPtr, SwapWorks) {
  wl::WlPtr<FakeProxyObj> a;
  wl::WlPtr<FakeProxyObj> b;
  a.Attach(reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0x1111)));
  b.Attach(reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0x2222)));
  a.Swap(b);
  EXPECT_EQ(a->GetProxy(),
            reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0x2222)));
  EXPECT_EQ(b->GetProxy(),
            reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0x1111)));
  a->Detach();
  b->Detach();
}
TEST(WlPtr, ResetCallsDestroy) {
  wl::WlPtr<FakeProxyObj> p;
  p.Attach(reinterpret_cast<wl_proxy*>(static_cast<uintptr_t>(0x9)));
  p.Reset();
  EXPECT_TRUE(p.IsNull());
}

// ── CRegistry basic tests (no compositor required) ───────────────────────────

TEST(CRegistry, DefaultIsNull) {
  wl::CRegistry r;
  EXPECT_TRUE(r.IsNull());
}
TEST(CRegistry, LambdaStored) {
  wl::CRegistry r;
  bool called = false;
  r.OnGlobal([&](wl::CRegistry&, uint32_t, std::string_view, uint32_t) {
    called = true;
  });
  EXPECT_FALSE(called);  // lambda stored but not called yet
}
TEST(CRegistry, RemoveStored) {
  wl::CRegistry r;
  bool called = false;
  r.OnRemove([&](wl::CRegistry&, uint32_t) { called = true; });
  EXPECT_FALSE(called);
}
