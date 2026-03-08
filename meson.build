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
expat_dep          = dependency('expat')

# ── Scanner library (shared between executable and tests) ─────────────────────

subdir('src')

# ── Tests ─────────────────────────────────────────────────────────────────────

if get_option('tests')
    subdir('tests')
endif

# ── Examples ─────────────────────────────────────────────────────────────────

if get_option('examples') and wayland_client_dep.found() and wayland_server_dep.found()
    subdir('examples')
endif
