# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# xdg-csd: client-side decoration example
#
# Demonstrates the zxdg_decoration_manager_v1 protocol for negotiating
# CSD vs SSD with the compositor, and renders client-side decorations
# (title bar with window buttons, resize borders) using a CSD plugin.
#
# Which plugin is built is controlled by the csd / csd_gtk options; with both
# left at 'auto' the highest-fidelity available one wins:
#   • GTK 3 present      → GTK-themed CSD plugin (GTK + Cairo + Pango)
#   • else Cairo present → Cairo CSD plugin (Cairo + Pango)
#   • otherwise          → fallback flat-color CSD plugin (no deps)

# ── Dep checks — silently skip if anything is absent ─────────────────────────

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)
xkbcommon_dep  = dependency('xkbcommon',         required: false)
wl_cursor_dep  = dependency('wayland-cursor',    required: false)

if not (wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found()
        and wl_cursor_dep.found())
    subdir_done()
endif

# ── Plugin dependency resolution ─────────────────────────────────────────────
#
# Two options decide what gets built and what gets used:
#   csd_gtk — which GTK the themed plugin is built against (or none at all)
#   csd     — which plugin the example links
#
# Naming either explicitly is a hard requirement: an unbuildable choice fails at
# configure time instead of quietly degrading to a lesser plugin.

csd_gtk_opt = get_option('csd_gtk')
csd_opt     = get_option('csd')

gtk_dep   = []
gtk_found = false

# 'ssd' outranks csd_gtk: with no plugin compiled there is no GTK backend to
# choose. Asking for both is a contradiction rather than a preference.
if csd_opt == 'ssd' and csd_gtk_opt in ['gtk3', 'gtk4']
    error('csd_gtk=' + csd_gtk_opt + ' selects a GTK backend, but csd=ssd ' +
          '(the default) builds no decoration plugin for it to back. ' +
          'Add -Dcsd=gtk to build the GTK plugin.')
endif

if csd_opt == 'ssd'
    # Skip GTK detection entirely: nothing will link it.
elif csd_gtk_opt == 'gtk4'
    error('csd_gtk=gtk4: the GTK 4 backend is not implemented. GTK 4 removed ' +
          'the offscreen widget-draw path this plugin renders through, so it ' +
          'needs a backend of its own. Use csd_gtk=gtk3 or csd_gtk=auto.')
elif csd_gtk_opt == 'gtk3'
    gtk_dep   = dependency('gtk+-3.0', required: true)
    gtk_found = true
elif csd_gtk_opt == 'auto'
    _gtk      = dependency('gtk+-3.0', required: false)
    gtk_found = _gtk.found()
    if gtk_found
        gtk_dep = _gtk
    endif
endif

cairo_dep      = []
pangocairo_dep = []
cairo_found    = false
if csd_opt != 'ssd'
    cairo_dep      = dependency('cairo',      required: csd_opt == 'cairo')
    pangocairo_dep = dependency('pangocairo', required: csd_opt == 'cairo')
    cairo_found    = cairo_dep.found() and pangocairo_dep.found()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml        = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml      = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_decoration_xml = wp_dep.get_variable('pkgdatadir') / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
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

xdg_decoration_client_hdr = custom_target(
    'xdg-csd-xdg-decoration-client-header',
    input:   xdg_decoration_xml,
    output:  'xdg_decoration_unstable_v1_client.hpp',
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
    xdg_decoration_client_hdr,
    xdg_csd_viewporter_client_hdr,
    xdg_csd_fractional_scale_client_hdr,
]

xdg_csd_deps = [wayland_client_dep, xkbcommon_dep, wl_cursor_dep]
xdg_csd_args = ['-Wno-unused-parameter']

# ── Plugin selection ─────────────────────────────────────────────────────────

csd_choice = csd_opt
if csd_choice == 'ssd'
    # No plugin, no define: the example asks for server-side decorations.
elif csd_choice == 'auto'
    # Highest fidelity available -- but the runtime still prefers the
    # compositor's own decorations, and only falls back to this plugin if the
    # compositor declines. Naming a plugin instead is what forces client-side.
    xdg_csd_args += '-DCSD_PREFER_SSD'
    if gtk_found
        csd_choice = 'gtk'
    elif cairo_found
        csd_choice = 'cairo'
    else
        csd_choice = 'fallback'
    endif
endif

if csd_choice == 'gtk'
    if not gtk_found
        if csd_gtk_opt == 'disabled'
            error('csd=gtk conflicts with csd_gtk=disabled: the GTK plugin was ' +
                  'asked for and gated off at the same time.')
        endif
        error('csd=gtk: gtk+-3.0 not found. Install it, or pick another plugin ' +
              'with csd=cairo / csd=fallback.')
    endif
    xdg_csd_sources += ['csd_gtk.cpp', 'csd_gtk_backend_gtk3.cpp']
    xdg_csd_deps    += gtk_dep
    xdg_csd_args    += '-DUSE_GTK_CSD'
elif csd_choice == 'cairo'
    if not cairo_found
        error('csd=cairo: cairo and pangocairo are both required.')
    endif
    xdg_csd_sources += 'csd_cairo.cpp'
    xdg_csd_deps    += [cairo_dep, pangocairo_dep]
    xdg_csd_args    += '-DUSE_CAIRO_CSD'
elif csd_choice == 'fallback'
    # Header-only: no source to add, but the define still has to say so, or
    # 'none' and 'fallback' would be indistinguishable to the example.
    xdg_csd_args    += '-DUSE_FALLBACK_CSD'
endif

message('xdg-csd: CSD plugin "' + csd_choice + '" (csd=' + csd_opt +
        ', csd_gtk=' + csd_gtk_opt + ')')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'xdg_csd',
    sources:            xdg_csd_sources,
    include_directories: [framework_inc, generated_inc],
    dependencies:       xdg_csd_deps,
    cpp_args:           xdg_csd_args,
)
