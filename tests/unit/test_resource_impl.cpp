// SPDX-License-Identifier: MIT
// Tests for wl::CResourceImpl<Derived, Traits>.
// These tests exercise the handle-management API without a live Wayland server.
#include <wl/resource_impl.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

// ── Minimal fake server-side traits ──────────────────────────────────────────

struct FakeServerTraits {
    static constexpr std::string_view interface_name = "wl_fake_server";
    static constexpr uint32_t         version        = 1;
    static const wl_interface& wl_iface() noexcept {
        static wl_interface s{};
        return s;
    }
};

// ── Minimal concrete implementation ──────────────────────────────────────────

struct FakeResource : wl::CResourceImpl<FakeResource, FakeServerTraits> {
    // s_request_vtable_ is required by CResourceImpl::_SetResource.
    // For tests we don't install a real vtable.
    static constexpr void* s_request_vtable_[] = {nullptr};

    int event_post_count = 0;
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(CResourceImpl, DefaultIsNull) {
    FakeResource r;
    EXPECT_TRUE(r.IsNull());
}

TEST(CResourceImpl, BoolConversionFalseWhenNull) {
    FakeResource r;
    EXPECT_FALSE(static_cast<bool>(r));
}

TEST(CResourceImpl, GetResourceReturnsNull) {
    FakeResource r;
    EXPECT_EQ(r.GetResource(), nullptr);
}

TEST(CResourceImpl, ProcessRequestDefaultReturnsFalse) {
    FakeResource r;
    EXPECT_FALSE(r.ProcessRequest(0, nullptr, nullptr, nullptr));
}
