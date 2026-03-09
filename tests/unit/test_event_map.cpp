// SPDX-License-Identifier: MIT
#include <wl/event_map.hpp>

#include <gtest/gtest.h>
#include <cstdint>

using namespace wl;

// ── Minimal base class using BEGIN_EVENT_MAP
// ──────────────────────────────────

class CBase : public CEventMap {
 public:
  int base_count = 0;

  template <typename T, typename Fn>
  static void _CrackEvent_10(T* self, void** args, Fn fn) {
    (self->*fn)(*reinterpret_cast<uint32_t*>(args[0]));
  }

  BEGIN_EVENT_MAP(CBase)
  EVENT_HANDLER(10, OnTen)
  END_EVENT_MAP()

  void OnTen(uint32_t v) { base_count += static_cast<int>(v); }
};

// ── Derived class that chains to base ────────────────────────────────────────

class CDerived : public CBase {
 public:
  bool saw_twenty = false;

  template <typename T, typename Fn>
  static void _CrackEvent_20(T* self, void** /*args*/, Fn fn) {
    uint32_t d = 0;
    (self->*fn)(d);
  }

  BEGIN_EVENT_MAP(CDerived)
  EVENT_HANDLER(20, OnTwenty)
  CHAIN_EVENT_MAP(CBase)
  END_EVENT_MAP()

  void OnTwenty(uint32_t) { saw_twenty = true; }
};

// ── Class with a member mixin ────────────────────────────────────────────────

class CMixinUser : public CEventMap {
 public:
  CBase m_mixin;
  bool own_handled = false;

  template <typename T, typename Fn>
  static void _CrackEvent_99(T* self, void** /*args*/, Fn fn) {
    uint32_t d = 0;
    (self->*fn)(d);
  }

  BEGIN_EVENT_MAP(CMixinUser)
  CHAIN_EVENT_MAP_MEMBER(m_mixin)
  EVENT_HANDLER(99, OnNN)
  END_EVENT_MAP()

  void OnNN(uint32_t) { own_handled = true; }
};

// ── Tests
// ─────────────────────────────────────────────────────────────────────

TEST(EventMap, HandlesOwnOpcode) {
  CBase b;
  uint32_t v = 7;
  void* a[1] = {&v};
  EXPECT_TRUE(b.ProcessEvent(10u, a));
  EXPECT_EQ(b.base_count, 7);
}
TEST(EventMap, RejectsUnknown) {
  CBase b;
  EXPECT_FALSE(b.ProcessEvent(99u, nullptr));
}
TEST(EventMap, DerivedOwnBeforeBase) {
  CDerived d;
  EXPECT_TRUE(d.ProcessEvent(20u, nullptr));
  EXPECT_TRUE(d.saw_twenty);
  EXPECT_EQ(d.base_count, 0);
}
TEST(EventMap, DerivedChainsToBase) {
  CDerived d;
  uint32_t v = 3;
  void* a[1] = {&v};
  EXPECT_TRUE(d.ProcessEvent(10u, a));
  EXPECT_EQ(d.base_count, 3);
}
TEST(EventMap, ChainMemberHandles) {
  CMixinUser u;
  uint32_t v = 5;
  void* a[1] = {&v};
  EXPECT_TRUE(u.ProcessEvent(10u, a));
  EXPECT_EQ(u.m_mixin.base_count, 5);
}
TEST(EventMap, OwnHandlerAfterMember) {
  CMixinUser u;
  EXPECT_TRUE(u.ProcessEvent(99u, nullptr));
  EXPECT_TRUE(u.own_handled);
}
