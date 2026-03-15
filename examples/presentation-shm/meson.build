# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# presentation-shm: C++23 port of Weston clients/presentation-shm.c
#
# Demonstrates the wp_presentation protocol for frame-timing feedback using
# wl_shm for an animated spinning-wheel pattern.
#
# Three run modes (selected by command-line flag):
#   -f  feedback     : frame callback → commit, print per-frame timings
#   -i  feedback-idle: same but sleep 1 s between frames
#   -p  low-lat      : presentation-feedback driven commit loop

# ── Dep checks — silently skip if anything is absent ─────────────────────────

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)

if not (wp_dep.found() and wl_scanner_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml           = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml         = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
presentation_time_xml = wp_dep.get_variable('pkgdatadir') / 'stable/presentation-time/presentation-time.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'presentation-shm-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'presentation-shm-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

presentation_time_client_hdr = custom_target(
    'presentation-shm-presentation-time-client-header',
    input:   presentation_time_xml,
    output:  'presentation_time_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'presentation_shm',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        presentation_time_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep],
    cpp_args: ['-Wno-unused-parameter'],
)
