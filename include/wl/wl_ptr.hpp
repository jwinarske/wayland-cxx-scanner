// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/proxy.hpp>

#include <cassert>

namespace wl {

/// RAII owning wrapper for a CProxy-derived object (≈ WTL CAutoPtr).
///
/// Calls T::Destroy() on the held object when the WlPtr is destroyed or reset.
/// T must be a type derived from CProxy<Traits> for some Traits, with a
/// public Destroy() method.
///
/// Design note: T is stored by value, but CProxy deliberately deletes its
/// copy/move constructors (it is a non-transferable handle).  WlPtr therefore
/// transfers ownership by swapping the underlying raw wl_proxy* via
/// Attach()/Detach() rather than by moving or copying T itself (R1).
///
/// Example:
///   wl::WlPtr<CMyXdgSurface> ptr;
///   ptr.Attach(raw_wl_proxy);   // take ownership
///   ptr->SendAck();
///
/// @tparam T  A CProxy-derived type exposing Attach(wl_proxy*), Detach(),
///            IsNull(), and Destroy().
template <typename T>
class WlPtr {
public:
    WlPtr() noexcept  = default;
    ~WlPtr() { Reset(); }

    /// Move constructor: transfers the raw wl_proxy* without invoking T's
    /// (deleted) move constructor (R1).
    WlPtr(WlPtr&& o) noexcept { m_obj.Attach(o.m_obj.Detach()); }

    /// Move assignment: destroys the current object then steals from @p o (R1).
    WlPtr& operator=(WlPtr&& o) noexcept {
        if (this != &o) {
            Reset();
            m_obj.Attach(o.m_obj.Detach());
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
    wl_proxy* Detach() noexcept { return m_obj.Detach(); }

    /// Destroy the held object (calls T::Destroy).
    void Reset() noexcept {
        if (!m_obj.IsNull())
            m_obj.Destroy();
    }

    [[nodiscard]] T*   operator->() noexcept { return &m_obj; }
    [[nodiscard]] T*   Get() noexcept { return &m_obj; }
    [[nodiscard]] bool IsNull() const noexcept { return m_obj.IsNull(); }
    explicit operator bool() const noexcept { return !IsNull(); }

    /// Swap ownership (R1: uses Attach/Detach, not std::swap on T).
    void Swap(WlPtr& o) noexcept {
        wl_proxy* tmp = m_obj.Detach();
        m_obj.Attach(o.m_obj.Detach());
        o.m_obj.Attach(tmp);
    }

private:
    T m_obj;
};

}  // namespace wl
