# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# ivi-shell: IVI-application client example.
#
# Demonstrates the ivi_application protocol: connects to a compositor that
# implements IVI-style surface management (e.g. Weston with the IVI shell
# plugin), creates a wl_surface backed by a shared-memory buffer, and
# registers it with an integer IVI surface ID via
# ivi_application.surface_create.
#
# The bundled protocols/ivi-application.xml provides the protocol definition;
# no system package is required beyond wayland-scanner.

# ── Dep checks — silently skip if wayland-scanner is absent ──────────────────

wl_scanner_dep = dependency('wayland-scanner', required: false)
xkbcommon_dep  = dependency('xkbcommon',       required: false)

if not (wl_scanner_dep.found() and xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate system wayland.xml and bundled protocol XML ───────────────────────

wayland_xml         = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
ivi_application_xml = meson.project_source_root() / 'protocols' / 'ivi-application.xml'

# ── Generate C++ client headers ───────────────────────────────────────────────

wayland_client_hdr = custom_target(
    'ivi-shell-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

ivi_application_client_hdr = custom_target(
    'ivi-shell-ivi-application-client-header',
    input:   ivi_application_xml,
    output:  'ivi_application_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'ivi_shell',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        ivi_application_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep, xkbcommon_dep],
    cpp_args: ['-Wno-unused-parameter'],
)
