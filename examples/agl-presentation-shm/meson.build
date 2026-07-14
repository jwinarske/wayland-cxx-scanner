# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# agl-compositor: AGL shell background client example.
#
# Demonstrates the agl_shell protocol alongside xdg-shell, as required by
# the AGL compositor: creates an xdg_surface/xdg_toplevel on top of the
# wl_surface (for the XDG configure flow), registers it as the background
# via agl_shell.set_background, then calls agl_shell.ready.
#
# The bundled protocols/agl-shell.xml provides the AGL protocol definition.
# xdg-shell.xml is obtained from the system wayland-protocols package.

# ── Dep checks — silently skip if any required dep is absent ─────────────────

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)
xkbcommon_dep  = dependency('xkbcommon',         required: false)

if not (wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate protocol XMLs ──────────────────────────────────────────────────────

wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
agl_shell_xml = meson.project_source_root() / 'protocols' / 'agl-shell.xml'

# ── Generate C++ client headers ───────────────────────────────────────────────

wayland_client_hdr = custom_target(
    'agl-compositor-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'agl-compositor-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

agl_shell_client_hdr = custom_target(
    'agl-compositor-agl-shell-client-header',
    input:   agl_shell_xml,
    output:  'agl_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'agl_compositor',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        agl_shell_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep, xkbcommon_dep],
    cpp_args: ['-Wno-unused-parameter'],
)
