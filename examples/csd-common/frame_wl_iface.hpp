// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// frame_wl_iface — inline wl_iface() for the interfaces the window frame binds
// beyond the two every windowed app already has (wl_compositor, wl_surface).
//
// The core wl_interface tables live in libwayland, not in a generated header,
// so wl_iface() must return the system symbol — which is why the repo defines
// these per consumer rather than emitting them from the scanner. Defined
// `inline` here they carry vague linkage, so this header can be included by any
// number of translation units — the frame's own decorated_window.cpp and every
// example that adopts it — without a duplicate-symbol clash, and each TU emits
// the ones it actually uses. Both sides must include it: the frame's TU emits
// wl_subcompositor/wl_subsurface (which it binds and an example never touches),
// and the example's TU emits wl_shm/wl_shm_pool/wl_buffer (which its own
// content buffers use). Between them every symbol is defined where it is
// needed.
#pragma once

#include "wayland_client.hpp"
extern "C" {
#include <wayland-client-protocol.h>
}

namespace wayland::client {

inline const wl_interface& wl_shm_traits::wl_iface() noexcept {
  return wl_shm_interface;
}
inline const wl_interface& wl_shm_pool_traits::wl_iface() noexcept {
  return wl_shm_pool_interface;
}
inline const wl_interface& wl_buffer_traits::wl_iface() noexcept {
  return wl_buffer_interface;
}
inline const wl_interface& wl_subcompositor_traits::wl_iface() noexcept {
  return wl_subcompositor_interface;
}
inline const wl_interface& wl_subsurface_traits::wl_iface() noexcept {
  return wl_subsurface_interface;
}
inline const wl_interface& wl_region_traits::wl_iface() noexcept {
  return wl_region_interface;
}

}  // namespace wayland::client
