# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# agl-compositor: AGL shell background client example.
#
# Demonstrates the agl_shell protocol: connects to an AGL compositor,
# creates a wl_surface backed by a shared-memory buffer, promotes it to
# the output background with agl_shell.set_background, then calls
# agl_shell.ready to signal the compositor that the shell is ready.
#
# The bundled protocols/agl-shell.xml provides the protocol definition;
# no system package is required beyond wayland-scanner.

# ── Dep checks — silently skip if wayland-scanner is absent ──────────────────

wl_scanner_dep = dependency('wayland-scanner', required: false)
xkbcommon_dep  = dependency('xkbcommon',       required: false)

if not (wl_scanner_dep.found() and xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate system wayland.xml ─────────────────────────────────────────────────

wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
agl_shell_xml = meson.project_source_root() / 'protocols' / 'agl-shell.xml'

# ── Generate C++ client headers ───────────────────────────────────────────────

wayland_client_hdr = custom_target(
    'agl-compositor-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

agl_shell_client_hdr = custom_target(
    'agl-compositor-agl-shell-client-header',
    input:   agl_shell_xml,
    output:  'agl_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'agl_compositor',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        agl_shell_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep, xkbcommon_dep],
    cpp_args: ['-Wno-unused-parameter'],
)
