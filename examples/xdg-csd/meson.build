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

if csd_gtk_opt == 'gtk4'
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

cairo_dep      = dependency('cairo',      required: csd_opt == 'cairo')
pangocairo_dep = dependency('pangocairo', required: csd_opt == 'cairo')
cairo_found    = cairo_dep.found() and pangocairo_dep.found()

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

xdg_csd_deps = [wayland_client_dep, xkbcommon_dep, wl_cursor_dep]
xdg_csd_args = ['-Wno-unused-parameter']

# ── Plugin selection ─────────────────────────────────────────────────────────

csd_choice = csd_opt
if csd_choice == 'auto'
    # Highest fidelity available.
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
    xdg_csd_sources += 'csd_gtk.cpp'
    xdg_csd_deps    += gtk_dep
    xdg_csd_args    += '-DUSE_GTK_CSD'
elif csd_choice == 'cairo'
    if not cairo_found
        error('csd=cairo: cairo and pangocairo are both required.')
    endif
    xdg_csd_sources += 'csd_cairo.cpp'
    xdg_csd_deps    += [cairo_dep, pangocairo_dep]
    xdg_csd_args    += '-DUSE_CAIRO_CSD'
endif
# 'fallback' adds no source: the plugin is header-only.

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
