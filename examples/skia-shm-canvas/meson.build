# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# skia-shm-canvas: renders the shared demo scene with Skia's CPU raster backend
# directly into a wl_shm buffer.  Descended into only when Skia was resolved
# (see examples/meson.build) and its deps are present.

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)
xkbcommon_dep  = dependency('xkbcommon',         required: false)
# The window frame draws a cursor over its own surface, through the framework's
# CursorManager (<decorated_frame.hpp>), which links wayland-cursor.
wl_cursor_dep  = dependency('wayland-cursor',    required: false)

if not (wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found()
        and wl_cursor_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wp_datadir            = wp_dep.get_variable('pkgdatadir')
wayland_xml           = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml         = wp_datadir / 'stable/xdg-shell/xdg-shell.xml'
viewporter_xml        = wp_datadir / 'stable/viewporter/viewporter.xml'
fractional_scale_xml  = wp_datadir / 'staging/fractional-scale/fractional-scale-v1.xml'
presentation_time_xml = wp_datadir / 'stable/presentation-time/presentation-time.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────
#
# viewporter and fractional-scale are extension protocols, so their headers are
# generated with --emit-interface-tables to stay self-contained (they define
# their own wl_interface tables and wl_iface()).

wayland_client_hdr = custom_target(
    'skia-shm-canvas-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'skia-shm-canvas-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

viewporter_client_hdr = custom_target(
    'skia-shm-canvas-viewporter-client-header',
    input:   viewporter_xml,
    output:  'viewporter_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

fractional_scale_client_hdr = custom_target(
    'skia-shm-canvas-fractional-scale-client-header',
    input:   fractional_scale_xml,
    output:  'fractional_scale_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

presentation_time_client_hdr = custom_target(
    'skia-shm-canvas-presentation-time-client-header',
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
    'skia_shm_canvas',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        viewporter_client_hdr,
        fractional_scale_client_hdr,
        presentation_time_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
        xkbcommon_dep,
        skia_demo_common_dep,
        # The window frame (csd_dep). With csd=none this is only the header path
        # and <decorated_frame.hpp> resolves to empty inlines.
        csd_dep,
        wl_cursor_dep,
    ],
    cpp_args: ['-Wno-unused-parameter'],
)
