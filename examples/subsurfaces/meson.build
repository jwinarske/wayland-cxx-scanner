# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# subsurfaces: C++23 port of Weston clients/subsurfaces.c
#
# Demonstrates wl_subsurface using mixed SHM + EGL surfaces:
#   • Main surface (green, SHM) — XDG toplevel
#   • Red subsurface (EGL, desync) — renders a spinning GL triangle,
#     position also oscillates independently of the parent
#   • Blue subsurface (SHM, sync) — commits only with the parent

# ── Dep checks — silently skip if anything is absent ─────────────────────────

egl_dep         = dependency('egl',               required: false)
glesv2_dep      = dependency('glesv2',            required: false)
wayland_egl_dep = dependency('wayland-egl',       required: false)
wp_dep          = dependency('wayland-protocols',  required: false)
wl_scanner_dep  = dependency('wayland-scanner',   required: false)
xkbcommon_dep   = dependency('xkbcommon',         required: false)

if not (egl_dep.found() and glesv2_dep.found() and wayland_egl_dep.found() and
        wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'subsurfaces-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'subsurfaces-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
# '.' picks up both the source dir and the build dir so that the generated
# wayland_client.hpp / xdg_shell_client.hpp are found at compile time.
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'subsurfaces',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep, egl_dep, glesv2_dep, wayland_egl_dep, xkbcommon_dep],
    cpp_args: ['-Wno-unused-parameter'],
)
