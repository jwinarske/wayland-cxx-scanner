// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/proxy.hpp>

extern "C" {
#include <wayland-server-core.h>
}

namespace wl {

/// CRTP base for generated server-side resource handler classes.
///
/// Wraps a wl_resource* and provides _PostEvent for sending events to the
/// client.  Generated classes use direct-dispatch static callbacks.
///
/// Generated server classes inherit from this:
///
///   template<class Derived>
///   class CXdgWmBaseServer : public wl::CResourceImpl<Derived,
///   xdg_wm_base_server_traits> { … };
///
/// @tparam Derived  The most-derived class (CRTP).
/// @tparam Traits   Server-side interface traits struct.
template <typename Derived, typename Traits>
class CResourceImpl {
 public:
  CResourceImpl() noexcept = default;
  virtual ~CResourceImpl() = default;

  /// Bind this handler to a newly created wl_resource.
  void _SetResource(wl_resource* resource) noexcept {
    m_resource = resource;
    if (resource) {
      wl_resource_set_implementation(resource, Derived::s_request_vtable_,
                                     static_cast<Derived*>(this),
                                     /*destructor=*/nullptr);
    }
  }

  [[nodiscard]] wl_resource* GetResource() const noexcept { return m_resource; }
  [[nodiscard]] bool IsNull() const noexcept { return m_resource == nullptr; }
  explicit operator bool() const noexcept { return !IsNull(); }

  /// Send an event to the client.
  template <typename... Args>
  void _PostEvent(uint32_t opcode, Args... args) noexcept {
    if (m_resource)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      wl_resource_post_event(m_resource, opcode, args...);
  }

 protected:
  wl_resource* m_resource = nullptr;
};

}  // namespace wl
