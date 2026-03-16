// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/proxy_impl.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

#include <algorithm>
#include <cstdint>

namespace wl {

/// Attach an already-created proxy to a WlPtr handler and install its event
/// listener.
///
/// Returns false (and leaves @p ptr unchanged) when @p raw is null, making it
/// safe to chain directly after a failed wl::construct<> or registry bind.
///
/// **Note:** This function does not set any application back-pointer (e.g.,
/// `app_` members used to dispatch events back to the owning class).  Set it
/// explicitly after a successful call when the handler requires it:
///
/// @code
///   if (!wl::SetupHandler(xdg_surface_,
///                         wl::construct<xdg_surface_traits,
///                                       xdg_wm_base_traits::Op::GetXdgSurface>(
///                             *xdg_wm_base_.Get(), surface_proxy))) {
///     // handle failure
///   }
///   xdg_surface_.Get()->app_ = this;  // set back-pointer when needed
/// @endcode
///
/// @tparam Handler  A CProxyImpl-derived type exposing _SetProxy().
template <typename Handler>
[[nodiscard]] inline bool SetupHandler(WlPtr<Handler>& ptr,
                                       wl_proxy* raw) noexcept {
  if (!raw)
    return false;
  ptr.Get()->_SetProxy(raw);
  return true;
}

/// Bind a registry global and set up the CRTP event handler in one call.
///
/// Binds the global identified by @p name at the minimum of @p ver and the
/// compile-time @p Traits::version, then delegates to SetupHandler().
///
/// Example:
/// @code
///   if (!wl::BindHandler<xdg_wm_base_traits>(registry_, xdg_wm_base_,
///                                             xdg_wm_base_name_,
///                                             xdg_wm_base_ver_)) {
///     // handle failure
///   }
/// @endcode
///
/// @tparam Traits   Interface traits satisfying WlProxyTraits.
/// @tparam Handler  A CProxyImpl-derived type exposing _SetProxy().
template <typename Traits, typename Handler>
[[nodiscard]] inline bool BindHandler(CRegistry& registry,
                                      WlPtr<Handler>& ptr,
                                      uint32_t name,
                                      uint32_t ver) noexcept {
  return SetupHandler(
      ptr, registry.Bind<Traits>(name, std::min(ver, Traits::version)));
}

}  // namespace wl
