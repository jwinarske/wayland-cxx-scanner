# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# agl-presentation-egl: AGL shell background client rendering through an FBO.
#
# Demonstrates the agl_shell background-client handshake (as agl-presentation-shm
# does) but renders with EGL / OpenGL ES 2: an animated scene is drawn into an
# offscreen framebuffer object, then composited to the window by texturing a
# full-screen quad.
#
# The bundled protocols/agl-shell.xml provides the AGL protocol definition;
# xdg-shell.xml comes from the system wayland-protocols package.

# ── Dep checks — silently skip if any required dep is absent ─────────────────

egl_dep         = dependency('egl',              required: false)
glesv2_dep      = dependency('glesv2',           required: false)
wayland_egl_dep = dependency('wayland-egl',      required: false)
wp_dep          = dependency('wayland-protocols', required: false)
wl_scanner_dep  = dependency('wayland-scanner',  required: false)
xkbcommon_dep   = dependency('xkbcommon',        required: false)

if not (egl_dep.found() and glesv2_dep.found() and
        wayland_egl_dep.found() and wp_dep.found() and wl_scanner_dep.found() and
        xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate protocol XMLs ──────────────────────────────────────────────────────

wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
agl_shell_xml = meson.project_source_root() / 'protocols' / 'agl-shell.xml'

# ── Generate C++ client headers ───────────────────────────────────────────────

wayland_client_hdr = custom_target(
    'agl-presentation-egl-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'agl-presentation-egl-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

agl_shell_client_hdr = custom_target(
    'agl-presentation-egl-agl-shell-client-header',
    input:   agl_shell_xml,
    output:  'agl_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'agl_presentation_egl',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        agl_shell_client_hdr,
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
