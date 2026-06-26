# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# simple-shell-egl: RDK/Westeros simple_shell EGL client example.
#
# Demonstrates the wl_simple_shell protocol: connects to a compositor that
# implements the RDK simple_shell (e.g. Westeros), creates a wl_surface
# rendered via EGL/OpenGL ES 2, receives the compositor-assigned surfaceId
# through the surface_id event, then drives surface layout directly via
# simple_shell.set_geometry / set_visible / set_zorder / set_opacity and logs
# the surface_status events the compositor replies with.
#
# The bundled protocols/simpleshell.xml provides the protocol definition; no
# system package is required beyond wayland-scanner. simple_shell does not use
# xdg-shell, so no wayland-protocols package is needed.

# ── Dep checks — silently skip if anything is absent ─────────────────────────

egl_dep         = dependency('egl',             required: false)
glesv2_dep      = dependency('glesv2',          required: false)
wayland_egl_dep = dependency('wayland-egl',     required: false)
wl_scanner_dep  = dependency('wayland-scanner', required: false)
xkbcommon_dep   = dependency('xkbcommon',       required: false)

if not (egl_dep.found() and glesv2_dep.found() and
        wayland_egl_dep.found() and wl_scanner_dep.found() and
        xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate system wayland.xml and bundled protocol XML ───────────────────────

wayland_xml      = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
simple_shell_xml = meson.project_source_root() / 'protocols' / 'simpleshell.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'simple-shell-egl-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

simple_shell_client_hdr = custom_target(
    'simple-shell-egl-simple-shell-client-header',
    input:   simple_shell_xml,
    output:  'simple_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
# '.' picks up both the source dir and the build dir, so the generated
# wayland_client.hpp / simple_shell_client.hpp are found at compile time.
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'simple_shell_egl',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        simple_shell_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
        egl_dep,
        glesv2_dep,
        wayland_egl_dep,
        xkbcommon_dep,
    ],
    # The generated headers use /* param */ comment-suppression for unused
    # virtual-method parameters, but the outer -Werror/-Wunused-parameter
    # flags can still fire on some compiler versions.
    cpp_args: ['-Wno-unused-parameter'],
)
