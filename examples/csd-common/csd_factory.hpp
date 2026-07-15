// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_factory — construct whichever decoration plugin the build selected.
//
// The csd option decides which plugin exists; this is where that decision is
// turned into an object, so no example has to know the answer. An example asks
// for a decoration and gets one, or gets null and goes undecorated.
#pragma once

#include <wl/csd_plugin.hpp>

#include <memory>

namespace wl::csd {

#ifdef CSD_ENABLED

/// Construct the plugin the build selected.
///
/// @returns null when the build selected no plugin (csd=ssd), and also when a
///          themed plugin declines at run time — GTK may be linked but have no
///          usable display or theme. A null plugin is a supported state, not an
///          error: the caller draws no decoration and asks the compositor to.
[[nodiscard]] std::unique_ptr<CsdPlugin> MakeCsdPlugin();

/// True when the build wants the compositor to decorate if it will, using the
/// plugin only if it declines (csd=auto). False when a plugin was named
/// explicitly, which means client-side is wanted.
[[nodiscard]] bool CsdPrefersServerSide() noexcept;

#else  // !CSD_ENABLED

/// csd=none: no decoration code is built, so there is nothing to construct and
/// nothing to link. Inline rather than absent, so a caller compiles unchanged.
[[nodiscard]] inline std::unique_ptr<CsdPlugin> MakeCsdPlugin() {
  return nullptr;
}

/// Nobody is asked to decorate, so there is no preference to express.
[[nodiscard]] inline bool CsdPrefersServerSide() noexcept {
  return false;
}

#endif  // CSD_ENABLED

}  // namespace wl::csd
