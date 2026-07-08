# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# ext-data-control: a focus-free clipboard CLI (copy / paste) over
# ext-data-control-v1.
#
# The windowed `clipboard` example drives the core wl_data_device family, which
# needs keyboard focus and an input serial.  ext-data-control lifts both
# restrictions, so the same wl::DataDevice helper — re-parameterized onto the
# ext-data-control trait bundle (wl::ExtDataControlProtocol) — becomes a
# headless wl-copy/wl-paste-style CLI.

# ── Dep checks — silently skip if anything is absent ─────────────────────────

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)

if not (wp_dep.found() and wl_scanner_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wp_datadir           = wp_dep.get_variable('pkgdatadir')
wayland_xml          = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
ext_data_control_xml = wp_datadir / 'staging/ext-data-control/ext-data-control-v1.xml'

# The staging protocol may predate this example's minimum; skip cleanly if the
# XML is not installed rather than failing configuration.
_fs = import('fs')
if not _fs.exists(ext_data_control_xml)
    warning('ext-data-control: ext-data-control-v1.xml not found in ' +
            'wayland-protocols; skipping.')
    subdir_done()
endif

# ── Generate C++ client headers from protocol XMLs ────────────────────────────
#
# ext-data-control is an extension protocol, so its header is generated with
# --emit-interface-tables to stay self-contained (it defines its own
# wl_interface tables and wl_iface()).

wayland_client_hdr = custom_target(
    'ext-data-control-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

ext_data_control_client_hdr = custom_target(
    'ext-data-control-client-header',
    input:   ext_data_control_xml,
    output:  'ext_data_control_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'ext-data-control',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        ext_data_control_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
    ],
    cpp_args: ['-Wno-unused-parameter'],
)
