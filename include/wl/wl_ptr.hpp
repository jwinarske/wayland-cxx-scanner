// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/proxy.hpp>

#include <cassert>
#include <utility>

namespace wl {

/// RAII owning wrapper for a CProxy-derived object (≈ WTL CAutoPtr).
///
/// Calls T::Destroy() on the held object when the WlPtr is destroyed or reset.
/// T must be a type derived from CProxy<Traits> for some Traits, with a
/// public Destroy() method.
///
/// Example:
///   wl::WlPtr<CMyXdgSurface> ptr;
///   ptr.Attach(raw_wl_proxy);   // take ownership
///   ptr->SendAck();
///
/// @tparam T  A CProxy-derived type with a Destroy() method.
template <typename T>
class WlPtr {
public:
    WlPtr() noexcept  = default;
    ~WlPtr() { Reset(); }

    WlPtr(WlPtr&& o) noexcept : m_obj(std::move(o.m_obj)) { o.m_obj.Attach(nullptr); }
    WlPtr& operator=(WlPtr&& o) noexcept {
        if (this != &o) {
            Reset();
            m_obj = std::move(o.m_obj);
            o.m_obj.Attach(nullptr);
        }
        return *this;
    }
    WlPtr(const WlPtr&)            = delete;
    WlPtr& operator=(const WlPtr&) = delete;

    /// Take ownership of @p p.  The WlPtr must currently be null.
    void Attach(wl_proxy* p) noexcept {
        assert(m_obj.IsNull() && "WlPtr::Attach on non-null pointer");
        m_obj.Attach(p);
    }

    /// Release ownership without destroying.
    [[nodiscard]] wl_proxy* Detach() noexcept { return m_obj.Detach(); }

    /// Destroy the held object (calls T::Destroy).
    void Reset() noexcept {
        if (!m_obj.IsNull())
            m_obj.Destroy();
    }

    [[nodiscard]] T*   operator->() noexcept { return &m_obj; }
    [[nodiscard]] T*   Get() noexcept { return &m_obj; }
    [[nodiscard]] bool IsNull() const noexcept { return m_obj.IsNull(); }
    explicit operator bool() const noexcept { return !IsNull(); }

    void Swap(WlPtr& o) noexcept { std::swap(m_obj, o.m_obj); }

private:
    T m_obj;
};

}  // namespace wl
