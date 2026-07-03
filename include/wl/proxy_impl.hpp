// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/proxy.hpp>

extern "C" {
#include <wayland-client-core.h>
}

namespace wl {

/// CRTP base for generated client protocol proxy classes.
///
/// Inherits the non-owning handle (CProxy).  Generated protocol classes
/// inherit from this:
///
///   template<class Derived>
///   class CXdgWmBase: public wl::CProxyImpl<Derived, xdg_wm_base_traits> { …
///   }
///
/// @tparam Derived  The most-derived class (CRTP).
/// @tparam Traits   Interface traits struct satisfying WlProxyTraits.
template <typename Derived, typename Traits>
class CProxyImpl : public CProxy<Traits> {
  using Base = CProxy<Traits>;

 public:
  virtual ~CProxyImpl() = default;

  /// Bind an already-created wl_proxy and install the static event listener.
  void _SetProxy(wl_proxy* proxy) noexcept {
    Base::Attach(proxy);
    if (proxy) {
      // s_listener_table_ is an inline static const void*[] of function
      // pointers.  wl_proxy_add_listener expects void(**)(void) — a pointer
      // to an array of void function pointers.  The reinterpret_cast is the
      // standard-compliant way to pass C function pointers through the
      // Wayland C API (S3: was previously cast to wl_dispatcher_func_t*
      // which is the wrong type).
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      wl_proxy_add_listener(
          proxy,
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
          reinterpret_cast<void (**)(void)>(
              const_cast<void**>(Derived::s_listener_table_)),
          this);
    }
  }

  /// Send a request to the compositor.
  /// Guard against null proxy (S4: calling with null is UB in the Wayland lib).
  template <typename... Args>
  void _Marshal(const uint32_t opcode, Args... args) noexcept {
    if (Base::m_proxy)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      wl_proxy_marshal(Base::m_proxy, opcode, args...);
  }

  /// Send a request that creates a new object and return the new proxy.
  /// Returns nullptr when the handle is null (S4).
  template <typename... Args>
  [[nodiscard]] wl_proxy* _MarshalNew(const uint32_t opcode,
                                      const wl_interface* iface,
                                      Args... args) noexcept {
    if (!Base::m_proxy)
      return nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return wl_proxy_marshal_constructor(Base::m_proxy, opcode, iface, args...);
  }

  /// Send a "bind"-style constructor request whose new_id carries no
  /// compile-time interface (wl_registry.bind semantics): the target interface
  /// and version are chosen at runtime.  Unlike a regular new_id, the dynamic
  /// new_id expands on the wire to (interface->name, version, id), so those
  /// three values are appended after @p pre — the wire arguments that precede
  /// the new_id slot (just @c name for wl_registry.bind).  The new proxy is
  /// created at the requested @p version (not the parent's), which is why the
  /// versioned constructor is required.  Returns nullptr when the handle is
  /// null (S4).
  template <typename... Pre>
  [[nodiscard]] wl_proxy* _MarshalBind(const uint32_t opcode,
                                       const wl_interface* iface,
                                       uint32_t version,
                                       Pre... pre) noexcept {
    if (!Base::m_proxy)
      return nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    return wl_proxy_marshal_constructor_versioned(Base::m_proxy, opcode, iface,
                                                  version, pre..., iface->name,
                                                  version, nullptr);
  }
};

// ── wl::construct<>
// ───────────────────────────────────────────────────────────

/// Type-safe wrapper for Wayland constructor requests — requests that create a
/// new server-side object and return its client-side proxy.
///
/// Encodes both the child interface type and the request opcode as compile-time
/// constants, making call sites self-documenting and letting the compiler
/// verify the opcode against the parent traits' Op struct.
///
/// The @c nullptr new_id placeholder is always implicit — every Wayland
/// constructor request follows this calling convention.
///
/// Example:
/// @code
///   // Before — verbose, easy-to-pass wrong interface or opcode:
///   parent._MarshalNew(xdg_wm_base_traits::Op::GetXdgSurface,
///                      &xdg_surface_traits::wl_iface(), nullptr, surface);
///
///   // After — child type and opcode baked in at compile time:
///   wl::construct<xdg_surface_traits,
///                 xdg_wm_base_traits::Op::GetXdgSurface>(parent, surface);
/// @endcode
///
/// @tparam ChildTraits   Traits of the newly created object (WlProxyTraits).
/// @tparam Opcode        The constructor request opcode (compile-time
/// constant).
/// @tparam Derived       Deduced — most-derived CRTP class of the parent proxy.
/// @tparam ParentTraits  Deduced — traits of the parent proxy.
/// @tparam Args          Deduced — extra wire arguments after the new_id slot.
template <typename ChildTraits,
          uint32_t Opcode,
          typename Derived,
          typename ParentTraits,
          typename... Args>
[[nodiscard]] wl_proxy* construct(CProxyImpl<Derived, ParentTraits>& proxy,
                                  Args... args) noexcept {
  static_assert(WlProxyTraits<ChildTraits>,
                "ChildTraits must satisfy wl::WlProxyTraits");
  return proxy._MarshalNew(Opcode, &ChildTraits::wl_iface(), nullptr, args...);
}

// ── wl::construct_at_end<>
// ─────────────────────────────────────────────────

/// Variant of @c wl::construct<> for requests where the new_id placeholder
/// appears **after** one or more leading wire arguments ("on", "uon", …).
///
/// The standard @c wl::construct<> emits @c nullptr before @p args, matching
/// the common "n…" signature.  This overload emits the extra arguments first,
/// then @c nullptr, matching signatures like "on" or "uon".
///
/// Example:
/// @code
///   // wp_presentation.feedback has wire signature "on" (surface, new_id):
///   wl::construct_at_end<wp_presentation_feedback_traits,
///                        wp_presentation_traits::Op::Feedback>(
///       presentation, surface.GetProxy());
/// @endcode
template <typename ChildTraits,
          uint32_t Opcode,
          typename Derived,
          typename ParentTraits,
          typename... Args>
[[nodiscard]] wl_proxy* construct_at_end(
    CProxyImpl<Derived, ParentTraits>& proxy,
    Args... args) noexcept {
  static_assert(WlProxyTraits<ChildTraits>,
                "ChildTraits must satisfy wl::WlProxyTraits");
  return proxy._MarshalNew(Opcode, &ChildTraits::wl_iface(), args..., nullptr);
}

// ── wl::bind<>
// ───────────────────────────────────────────────────────────────

/// Type-safe wrapper for a dynamic bind request (wl_registry.bind), whose
/// new_id has no compile-time interface — the target interface and version are
/// chosen at runtime.  This is the dynamic-new_id analogue of @c wl::construct:
/// @p BindTraits selects the interface table and @p Opcode is the bind opcode.
///
/// Example:
/// @code
///   wl_proxy* compositor =
///       wl::bind<wl_compositor_traits, wl_registry_traits::Op::Bind>(
///           registry, name, version);
/// @endcode
///
/// @tparam BindTraits    Traits of the object to bind (WlProxyTraits).
/// @tparam Opcode        The bind request opcode (compile-time constant).
/// @tparam Derived       Deduced — most-derived CRTP class of the parent proxy.
/// @tparam ParentTraits  Deduced — traits of the parent (registry) proxy.
template <typename BindTraits,
          uint32_t Opcode,
          typename Derived,
          typename ParentTraits>
[[nodiscard]] wl_proxy* bind(CProxyImpl<Derived, ParentTraits>& proxy,
                             uint32_t name,
                             uint32_t version) noexcept {
  static_assert(WlProxyTraits<BindTraits>,
                "BindTraits must satisfy wl::WlProxyTraits");
  return proxy._MarshalBind(Opcode, &BindTraits::wl_iface(), version, name);
}

}  // namespace wl
