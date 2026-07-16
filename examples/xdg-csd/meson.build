# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# xdg-csd: a decorated window, drawn with wl_shm
#
# The frame is wl::csd::DecoratedWindow, which csd_dep carries along with the
# plugin that draws it and the xdg-decoration codegen it negotiates with. Which
# plugin that is, is the csd / csd_gtk options' business — see
# examples/csd-common. This example only asks for a decoration.

# ── Dep checks — silently skip if anything is absent ─────────────────────────

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)
xkbcommon_dep  = dependency('xkbcommon',         required: false)
wl_cursor_dep  = dependency('wayland-cursor',    required: false)

if not (wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found()
        and wl_cursor_dep.found())
    subdir_done()
endif

# This example exists to demonstrate client-side decoration and negotiates the
# mode itself, so a build that asked for no decoration code at all has no use
# for it. Skipped rather than built inert.
if get_option('csd') == 'none'
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml        = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml      = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
viewporter_xml       = wp_dep.get_variable('pkgdatadir') / 'stable/viewporter/viewporter.xml'
fractional_scale_xml = wp_dep.get_variable('pkgdatadir') / 'staging/fractional-scale/fractional-scale-v1.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'xdg-csd-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'xdg-csd-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# viewporter and fractional-scale are extension protocols, so their headers are
# generated with --emit-interface-tables to stay self-contained (they define
# their own wl_interface tables and wl_iface()).
xdg_csd_viewporter_client_hdr = custom_target(
    'xdg-csd-viewporter-client-header',
    input:   viewporter_xml,
    output:  'viewporter_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

xdg_csd_fractional_scale_client_hdr = custom_target(
    'xdg-csd-fractional-scale-client-header',
    input:   fractional_scale_xml,
    output:  'fractional_scale_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Build sources and flags ───────────────────────────────────────────────────

xdg_csd_sources = [
    'main.cpp',
    wayland_client_hdr,
    xdg_shell_client_hdr,
    xdg_csd_viewporter_client_hdr,
    xdg_csd_fractional_scale_client_hdr,
]

# csd_dep carries the decoration plugin, its toolkit dependency and the define
# that says which one it is — all decided by the csd option in
# examples/csd-common. This example only has to ask for a decoration.
xdg_csd_deps = [wayland_client_dep, xkbcommon_dep, wl_cursor_dep, csd_dep]
xdg_csd_args = ['-Wno-unused-parameter']

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'xdg_csd',
    sources:            xdg_csd_sources,
    include_directories: [framework_inc, generated_inc],
    dependencies:       xdg_csd_deps,
    cpp_args:           xdg_csd_args,
)
