# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

project(
    'wayland-cxx-scanner',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>= 1.1',
    default_options: [
        'cpp_std=c++23',
        'warning_level=3',
        'werror=true',
        'b_ndebug=if-release',
    ],
)

# ── Dependencies ──────────────────────────────────────────────────────────────

wayland_client_dep = dependency('wayland-client', required: false)
wayland_server_dep = dependency('wayland-server', required: false)
pugixml_dep        = dependency('pugixml',
    required: true,
    fallback: ['pugixml', 'pugixml_dep'],
)

# ── Framework headers — install to ${includedir}/wl/ ─────────────────────────

install_headers(
    'include/wl/fd_handle.hpp',
    'include/wl/raii.hpp',
    'include/wl/event_map.hpp',
    'include/wl/proxy.hpp',
    'include/wl/proxy_impl.hpp',
    'include/wl/wl_ptr.hpp',
    'include/wl/resource_impl.hpp',
    'include/wl/registry.hpp',
    'include/wl/wayland.hpp',
    subdir: 'wl',
)

# ── pkg-config for downstream consumers of the framework headers ──────────────

pkg = import('pkgconfig')
pkg.generate(
    name: 'wayland-cxx',
    description: 'WTL-patterned C++23 Wayland framework headers',
    version: meson.project_version(),
    requires: [],      # header-only; consumers link wayland-client/server themselves
)

# ── Scanner tool and library ──────────────────────────────────────────────────

subdir('src')

# ── Tests ─────────────────────────────────────────────────────────────────────

if get_option('tests')
    subdir('tests')
endif

# ── Examples ─────────────────────────────────────────────────────────────────

if get_option('examples') and wayland_client_dep.found() and wayland_server_dep.found()
    subdir('examples')
endif
