// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/event_map.hpp>
#include <wl/proxy.hpp>

extern "C" {
#include <wayland-client-core.h>
}

namespace wl {

/// CRTP base for generated client protocol proxy classes.
///
/// Inherits both the non-owning handle (CProxy) and the event-dispatch base
/// (CEventMap).  Generated protocol classes inherit from this:
///
///   template<class Derived>
///   class CXdgWmBase : public wl::CProxyImpl<Derived, xdg_wm_base_traits> { … };
///
/// @tparam Derived  The most-derived class (CRTP).
/// @tparam Traits   Interface traits struct satisfying WlProxyTraits.
template <typename Derived, typename Traits>
    requires WlProxyTraits<Traits>
class CProxyImpl : public CProxy<Traits>, public CEventMap {
    using Base = CProxy<Traits>;

public:
    /// Bind an already-created wl_proxy and install the static event listener.
    void _SetProxy(wl_proxy* proxy) noexcept {
        Base::Attach(proxy);
        if (proxy)
            wl_proxy_add_listener(
                proxy,
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                reinterpret_cast<wl_dispatcher_func_t*>(
                    const_cast<void**>(Derived::s_listener_table_)),
                this);
    }

    /// Send a request to the compositor.
    template <typename... Args>
    void _Marshal(uint32_t opcode, Args... args) noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        wl_proxy_marshal(Base::m_proxy, opcode, args...);
    }

    /// Send a request that creates a new object and return the new proxy.
    template <typename... Args>
    [[nodiscard]] wl_proxy* _MarshalNew(uint32_t opcode, const wl_interface* iface,
                                        Args... args) noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        return wl_proxy_marshal_constructor(Base::m_proxy, opcode, iface, args...);
    }
};

}  // namespace wl
