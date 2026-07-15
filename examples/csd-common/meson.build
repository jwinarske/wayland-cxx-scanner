# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# csd-common: the client-side decoration plugins, shared by any example that
# wants a window frame. Add `csd_dep` to an example's dependencies and include
# <wl/csd_plugin.hpp>; which plugin is behind it is the csd option's business,
# not the example's.
#
# These live here rather than in include/wl/ because they cannot be framework
# headers: the themed plugins need a .cpp and a toolkit on the link line, and
# the framework is header-only. What *is* installable stays there —
# <wl/csd_plugin.hpp> (the interface), <wl/csd_fallback.hpp> (header-only, no
# dependencies) and <wl/csd_common.hpp> (color helpers).
#
# How the decoration is chosen:
#
#   ssd       no plugin at all; the example asks the compositor to decorate
#   auto      the highest-fidelity plugin available, but still prefer the
#             compositor at run time
#   gtk       |
#   cairo     | that plugin, forcing client-side
#   fallback  |
#
# Naming a plugin that cannot be built is a configure error rather than a
# silent downgrade to a lesser one.

csd_gtk_opt = get_option('csd_gtk')
csd_opt     = get_option('csd')

# ── Dependency resolution ────────────────────────────────────────────────────

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

# ── Plugin selection ─────────────────────────────────────────────────────────

csd_choice   = csd_opt
csd_sources  = []
csd_deps     = []
csd_args     = []

if csd_choice == 'ssd'
    # No plugin, no define: the example asks for server-side decorations.
elif csd_choice == 'auto'
    # Highest fidelity available -- but the runtime still prefers the
    # compositor's own decorations, and only falls back to this plugin if the
    # compositor declines. Naming a plugin instead is what forces client-side.
    csd_args += '-DCSD_PREFER_SSD'
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
    csd_sources += ['csd_gtk.cpp', 'csd_gtk_backend_gtk3.cpp']
    csd_deps    += gtk_dep
    csd_args    += '-DUSE_GTK_CSD'
elif csd_choice == 'cairo'
    if not cairo_found
        error('csd=cairo: cairo and pangocairo are both required.')
    endif
    csd_sources += 'csd_cairo.cpp'
    csd_deps    += [cairo_dep, pangocairo_dep]
    csd_args    += '-DUSE_CAIRO_CSD'
elif csd_choice == 'fallback'
    # Header-only: no source to add, but the define still has to say so, or
    # 'ssd' and 'fallback' would be indistinguishable to the consumer.
    csd_args    += '-DUSE_FALLBACK_CSD'
endif

message('csd-common: CSD plugin "' + csd_choice + '" (csd=' + csd_opt +
        ', csd_gtk=' + csd_gtk_opt + ')')

# ── The dependency ───────────────────────────────────────────────────────────

csd_common_inc = include_directories('.')
framework_inc_csd = include_directories('../../include')

# 'ssd' and 'fallback' compile nothing, so there is no library to link — the
# dependency then carries only the include path and the define.
if csd_sources.length() > 0
    csd_common_lib = static_library(
        'csd_common',
        sources: csd_sources,
        include_directories: [csd_common_inc, framework_inc_csd],
        dependencies: csd_deps,
        cpp_args: csd_args,
    )
    csd_dep = declare_dependency(
        link_with: csd_common_lib,
        include_directories: csd_common_inc,
        dependencies: csd_deps,
        compile_args: csd_args,
    )
else
    csd_dep = declare_dependency(
        include_directories: csd_common_inc,
        compile_args: csd_args,
    )
endif
