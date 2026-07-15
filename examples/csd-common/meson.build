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

# ── The window frame ─────────────────────────────────────────────────────────
#
# DecoratedWindow puts the decoration on a subsurface, which is what lets an EGL
# or Vulkan client be decorated at all: it has no CPU buffer of its own for a
# plugin to paint into. It is built whatever the csd option says, because it
# owns the decoration negotiation — with no plugin that negotiation is what asks
# the compositor to decorate, which is precisely the csd=ssd case.
#
# It speaks the protocol, so unlike the plugins it needs the scanner and the
# protocol XMLs. Every example that would take it needs those too and skips
# itself without them, so their absence costs nothing here but the frame.

wp_dep_csd         = dependency('wayland-protocols', required: false)
wl_scanner_dep_csd = dependency('wayland-scanner',   required: false)

csd_frame_ok = (wp_dep_csd.found() and wl_scanner_dep_csd.found()
                and wayland_client_dep.found())

# csd_factory is pure C++ and always builds: it is where the csd option's
# defines are read, and reading them is the whole point of the option.
csd_sources += 'csd_factory.cpp'

if csd_frame_ok
    csd_wayland_xml_f        = wl_scanner_dep_csd.get_variable('pkgdatadir') / 'wayland.xml'
    csd_xdg_shell_xml_f      = wp_dep_csd.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
    csd_xdg_decoration_xml_f = wp_dep_csd.get_variable('pkgdatadir') / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'

    csd_wayland_client_hdr = custom_target(
        'csd-common-wayland-client-header',
        input:   csd_wayland_xml_f,
        output:  'wayland_client.hpp',
        command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
    )
    csd_xdg_shell_client_hdr = custom_target(
        'csd-common-xdg-shell-client-header',
        input:   csd_xdg_shell_xml_f,
        output:  'xdg_shell_client.hpp',
        command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
    )
    csd_xdg_decoration_client_hdr = custom_target(
        'csd-common-xdg-decoration-client-header',
        input:   csd_xdg_decoration_xml_f,
        output:  'xdg_decoration_unstable_v1_client.hpp',
        command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
    )

    csd_sources += [
        'decorated_window.cpp',
        csd_wayland_client_hdr,
        csd_xdg_shell_client_hdr,
        csd_xdg_decoration_client_hdr,
    ]
    csd_deps += wayland_client_dep
    csd_args += '-DCSD_HAVE_DECORATED_WINDOW'
else
    message('csd-common: no wayland-protocols/scanner — building the plugins ' +
            'without the subsurface window frame')
endif

# ── The dependency ───────────────────────────────────────────────────────────

csd_common_inc = include_directories('.')
framework_inc_csd = include_directories('../../include')

# There is always something to build — csd_factory at least — so unlike the
# plugins this is unconditional. The generated headers land in this target's
# build directory, which csd_common_inc covers, so decorated_window.cpp includes
# them by bare name the way an example does.
#
# The library deliberately leaves wl_iface() for the core wl_* interfaces
# undefined: this repo defines those per-consumer, so defining them here would
# collide with the very example linking this. The link is what supplies them,
# and omitting one is a link error naming exactly what is missing.
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
