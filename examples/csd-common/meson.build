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
#   none      nothing at all is compiled or linked -- no plugin, no window
#             frame, no decoration protocol. For targets that will never want a
#             client-drawn frame, where the shell owns the screen. Consumers
#             still compile unchanged: <decorated_window.hpp> then defines a
#             window whose decoration calls are empty inlines.
#   ssd       no plugin, but the frame is built so it can ask the compositor to
#             decorate. This is what gets a title bar out of a compositor that
#             draws its own.
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

# 'none' and 'ssd' outrank csd_gtk: with no plugin compiled there is no GTK
# backend to choose. Asking for both is a contradiction rather than a
# preference.
if csd_opt in ['none', 'ssd'] and csd_gtk_opt in ['gtk3', 'gtk4']
    error('csd_gtk=' + csd_gtk_opt + ' selects a GTK backend, but csd=' +
          csd_opt + ' builds no decoration plugin for it to back. ' +
          'Add -Dcsd=gtk to build the GTK plugin.')
endif

if csd_opt in ['none', 'ssd']
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
if csd_opt not in ['none', 'ssd']
    cairo_dep      = dependency('cairo',      required: csd_opt == 'cairo')
    pangocairo_dep = dependency('pangocairo', required: csd_opt == 'cairo')
    cairo_found    = cairo_dep.found() and pangocairo_dep.found()
endif

# ── Plugin selection ─────────────────────────────────────────────────────────

csd_choice   = csd_opt
csd_sources  = []
csd_deps     = []
csd_args     = []

if csd_choice in ['none', 'ssd']
    # No plugin, no plugin define. 'none' goes further and builds nothing at
    # all, which is handled below.
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
# plugin to paint into. It is built for every choice but 'none', because it owns
# the decoration negotiation -- with no plugin that negotiation is what asks the
# compositor to decorate, which is precisely the csd=ssd case.
#
# It speaks the protocol, so unlike the plugins it needs the scanner and the
# protocol XMLs. Every example that would take it needs those too and skips
# itself without them, so their absence costs nothing here but the frame.

if csd_opt == 'none'
    # Nothing is compiled and nothing is linked -- no library, no protocol
    # headers, no toolkit, not even the frame. The consumer's include of
    # <decorated_window.hpp> resolves to empty inlines because CSD_ENABLED is
    # absent, so it compiles unchanged and carries none of this.
    message('csd-common: no decoration (csd=none) -- nothing built')
    csd_dep = declare_dependency(
        include_directories: include_directories('.'),
    )
    subdir_done()
endif

wp_dep_csd         = dependency('wayland-protocols', required: false)
wl_scanner_dep_csd = dependency('wayland-scanner',   required: false)

csd_frame_ok = (wp_dep_csd.found() and wl_scanner_dep_csd.found()
                and wayland_client_dep.found())

# csd_factory is pure C++ and always builds here: it is where the csd option's
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

    # The decoration surface's viewport, which is what lets it honor a
    # fractional scale. An extension protocol, so --emit-interface-tables keeps
    # the header self-contained: it defines its own wl_interface tables and
    # wl_iface() bodies, all inline, so an example generating a copy of its own
    # collides with neither.
    csd_viewporter_client_hdr = custom_target(
        'csd-common-viewporter-client-header',
        input:   wp_dep_csd.get_variable('pkgdatadir') / 'stable/viewporter/viewporter.xml',
        output:  'viewporter_client.hpp',
        command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
                  '@INPUT@', '@OUTPUT@'],
    )

    csd_sources += [
        'decorated_window.cpp',
        csd_wayland_client_hdr,
        csd_xdg_shell_client_hdr,
        csd_xdg_decoration_client_hdr,
        csd_viewporter_client_hdr,
    ]
    csd_deps += wayland_client_dep
    # Says the library exists to link. Without it every consumer's
    # <decorated_window.hpp> and <csd_factory.hpp> resolve to empty inlines, so
    # a missing scanner degrades to an undecorated window rather than a
    # configure error.
    csd_args += '-DCSD_ENABLED'
else
    message('csd-common: no wayland-protocols/scanner — building the plugins ' +
            'without the subsurface window frame')
endif

# ── The dependency ───────────────────────────────────────────────────────────

csd_common_inc = include_directories('.')
framework_inc_csd = include_directories('../../include')

# There is always something to build here — csd_factory at least — because
# csd=none returned above. The generated headers land in this target's build
# directory, which csd_common_inc covers, so decorated_window.cpp includes them
# by bare name the way an example does.
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
