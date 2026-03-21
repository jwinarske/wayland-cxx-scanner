# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# xdg-csd: C++23 client-side decoration example
#
# Demonstrates the zxdg_decoration_manager_v1 protocol for negotiating
# CSD vs SSD with the compositor, and renders client-side decorations
# (title bar with window buttons, resize borders) using a CSD plugin.
#
# Plugin selection follows the libdecor pattern (highest-fidelity wins):
#   • If GTK 3 is found  → GTK-themed CSD plugin (GTK + Cairo + Pango)
#   • Elif Cairo is found → Cairo CSD plugin (Cairo + Pango, libdecor style)
#   • Otherwise           → fallback flat-color CSD plugin (no deps)

# ── Dep checks — silently skip if anything is absent ─────────────────────────

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)
xkbcommon_dep  = dependency('xkbcommon',         required: false)

if not (wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found())
    subdir_done()
endif

# ── Optional plugin dependency detection ─────────────────────────────────────

gtk_dep       = dependency('gtk+-3.0',   required: false)
cairo_dep     = dependency('cairo',      required: false)
pangocairo_dep = dependency('pangocairo', required: false)

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml        = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml      = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_decoration_xml = wp_dep.get_variable('pkgdatadir') / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'

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

xdg_decoration_client_hdr = custom_target(
    'xdg-csd-xdg-decoration-client-header',
    input:   xdg_decoration_xml,
    output:  'xdg_decoration_unstable_v1_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Build sources and flags ───────────────────────────────────────────────────

xdg_csd_sources = [
    'main.cpp',
    wayland_client_hdr,
    xdg_shell_client_hdr,
    xdg_decoration_client_hdr,
]

xdg_csd_deps = [wayland_client_dep, xkbcommon_dep]
xdg_csd_args = ['-Wno-unused-parameter']

if gtk_dep.found()
    xdg_csd_sources += 'csd_gtk.cpp'
    xdg_csd_deps    += gtk_dep
    xdg_csd_args    += '-DUSE_GTK_CSD'
    message('xdg-csd: GTK 3 found → building with GTK CSD plugin')
elif cairo_dep.found() and pangocairo_dep.found()
    xdg_csd_sources += 'csd_cairo.cpp'
    xdg_csd_deps    += [cairo_dep, pangocairo_dep]
    xdg_csd_args    += '-DUSE_CAIRO_CSD'
    message('xdg-csd: Cairo + Pango found → building with Cairo CSD plugin')
else
    message('xdg-csd: no Cairo/GTK → building with fallback CSD plugin')
endif

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'xdg_csd',
    sources:            xdg_csd_sources,
    include_directories: [framework_inc, generated_inc],
    dependencies:       xdg_csd_deps,
    cpp_args:           xdg_csd_args,
)
