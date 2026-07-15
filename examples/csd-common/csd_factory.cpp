// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_factory — see csd_factory.hpp.
//
// The one place that knows which plugin the build selected. Keeping it here
// rather than in each example means the defines the csd option sets are read
// once, and an example that wants a decoration just asks for one.

#include "csd_factory.hpp"

// Exactly one plugin is compiled in, and with csd=ssd there is none. The
// fallback comes along with the GTK plugin regardless: it is the run-time
// landing spot when GTK declines to start.
#ifdef USE_GTK_CSD
#include "csd_gtk.hpp"

#include <wl/csd_fallback.hpp>
#elif defined(USE_CAIRO_CSD)
#include "csd_cairo.hpp"
#elif defined(USE_FALLBACK_CSD)
#include <wl/csd_fallback.hpp>
#endif

#include <cstdio>

namespace wl::csd {

std::unique_ptr<CsdPlugin> MakeCsdPlugin() {
#ifdef USE_GTK_CSD
  if (auto gtk = GtkCsdPlugin::TryCreate()) {
    std::fprintf(stderr, "csd: GTK plugin\n");
    return gtk;
  }
  // GTK is linked but unusable — no display, no theme. Degrade rather than
  // abort: the fallback needs nothing at all.
  std::fprintf(stderr, "csd: GTK unavailable at run time — fallback plugin\n");
  return std::make_unique<FallbackCsdPlugin>();
#elif defined(USE_CAIRO_CSD)
  std::fprintf(stderr, "csd: Cairo plugin\n");
  return std::make_unique<CairoCsdPlugin>();
#elif defined(USE_FALLBACK_CSD)
  std::fprintf(stderr, "csd: fallback plugin\n");
  return std::make_unique<FallbackCsdPlugin>();
#else
  std::fprintf(stderr, "csd: no plugin (server-side decorations)\n");
  return nullptr;
#endif
}

bool CsdPrefersServerSide() noexcept {
#ifdef CSD_PREFER_SSD
  return true;
#else
  return false;
#endif
}

}  // namespace wl::csd
