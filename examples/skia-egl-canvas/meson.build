# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# skia-egl-canvas: renders the shared demo scene with Skia's Ganesh GL backend
# through an EGL / wayland-egl surface.  Descended into only when Skia was
# resolved (see examples/meson.build) and its EGL/GLES deps are present.

egl_dep         = dependency('egl',               required: false)
glesv2_dep      = dependency('glesv2',            required: false)
wayland_egl_dep = dependency('wayland-egl',       required: false)
wp_dep          = dependency('wayland-protocols', required: false)
wl_scanner_dep  = dependency('wayland-scanner',   required: false)
xkbcommon_dep   = dependency('xkbcommon',         required: false)
# The window frame's cursor loads its theme through wayland-cursor.
wl_cursor_dep   = dependency('wayland-cursor',    required: false)

if not (egl_dep.found() and glesv2_dep.found() and wayland_egl_dep.found() and
        wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found() and
        wl_cursor_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml           = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml         = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
presentation_time_xml = wp_dep.get_variable('pkgdatadir') / 'stable/presentation-time/presentation-time.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'skia-egl-canvas-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'skia-egl-canvas-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# Extension protocol → generated with --emit-interface-tables so it defines its
# own wl_interface tables and wl_iface() (presentation-time is not in libwayland).
presentation_time_client_hdr = custom_target(
    'skia-egl-canvas-presentation-time-client-header',
    input:   presentation_time_xml,
    output:  'presentation_time_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'skia_egl_canvas',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        presentation_time_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
        egl_dep,
        glesv2_dep,
        wayland_egl_dep,
        xkbcommon_dep,
        skia_demo_common_dep,
        # The window frame (csd_dep). With csd=none this is only the header path.
        csd_dep,
        wl_cursor_dep,
    ],
    cpp_args: ['-Wno-unused-parameter'],
)
