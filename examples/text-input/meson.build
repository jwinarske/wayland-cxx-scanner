# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# text-input: Scenario 1 — a wl_shm text-field window that drives the
# wl::ime::ITextInputReceiver facade (the build-selected backend) and renders
# the committed + preedit text the compositor's IME sends back.

# Built for the text-input receiver backends.  backend.hpp resolves to the
# selected wrapper, which includes the matching generated header produced below.
ime_backend = get_option('ime_backend')
if ime_backend not in ['text-input-v1', 'text-input-v3']
    subdir_done()
endif

wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',  required: false)
if not (wp_dep.found() and wl_scanner_dep.found())
    subdir_done()
endif

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

if ime_backend == 'text-input-v3'
    ti_xml = wp_dep.get_variable('pkgdatadir') / 'unstable/text-input/text-input-unstable-v3.xml'
    ti_out = 'text_input_v3_client.hpp'
else
    ti_xml = wp_dep.get_variable('pkgdatadir') / 'unstable/text-input/text-input-unstable-v1.xml'
    ti_out = 'text_input_v1_client.hpp'
endif

wayland_client_hdr = custom_target(
    'text-input-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'text-input-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# The backend header includes this by a fixed name (text_input_v{1,3}_client.hpp);
# --emit-interface-tables makes it self-contained (defines wl_iface()).
text_input_client_hdr = custom_target(
    'text-input-client-header',
    input:   ti_xml,
    output:  ti_out,
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'text_input',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        text_input_client_hdr,
    ],
    include_directories: [
        include_directories('../../include'),
        include_directories('.'),
    ],
    dependencies: [wayland_client_dep],
    cpp_args: ['-Wno-unused-parameter'],
)
