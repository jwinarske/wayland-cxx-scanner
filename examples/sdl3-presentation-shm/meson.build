# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# sdl3-presentation-shm: SDL3 window with wp_presentation frame-timing feedback
#
# Demonstrates using the wayland-cxx-scanner generated C++ bindings for an
# external protocol (presentation-time) alongside SDL3's window management
# and 2D rendering.  SDL3 owns the wl_display, wl_surface, and xdg_toplevel;
# we extract the raw Wayland objects via SDL3 properties and bind
# wp_presentation ourselves for accurate per-frame timing.

# ── Dep checks — silently skip if anything is absent ─────────────────────────

sdl3_dep       = dependency('sdl3', required: false)
wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)

if not (sdl3_dep.found() and wp_dep.found() and wl_scanner_dep.found())
    subdir_done()
endif

# ── Locate the presentation-time protocol XML ───────────────────────────────

presentation_time_xml = wp_dep.get_variable('pkgdatadir') / 'stable/presentation-time/presentation-time.xml'

# ── Generate C++ client header from protocol XML ────────────────────────────

presentation_time_client_hdr = custom_target(
    'sdl3-presentation-shm-presentation-time-client-header',
    input:   presentation_time_xml,
    output:  'presentation_time_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'sdl3_presentation_shm',
    sources: [
        'main.cpp',
        presentation_time_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep, sdl3_dep],
    cpp_args: ['-Wno-unused-parameter'],
)
