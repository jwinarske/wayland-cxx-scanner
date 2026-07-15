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

/// Construct the plugin the build selected.
///
/// @returns null when the build selected none (csd=ssd), and also when a
///          themed plugin declines at run time — GTK may be linked but have no
///          usable display or theme. A null plugin is a supported state, not an
///          error: the caller draws no decoration and asks the compositor to.
[[nodiscard]] std::unique_ptr<CsdPlugin> MakeCsdPlugin();

/// True when the build wants the compositor to decorate if it will, using the
/// plugin only if it declines (csd=auto). False when a plugin was named
/// explicitly, which means client-side is wanted.
[[nodiscard]] bool CsdPrefersServerSide() noexcept;

}  // namespace wl::csd
