# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

gtest_dep      = dependency('gtest',      required: false, fallback: ['gtest', 'gtest_dep'])
gtest_main_dep = dependency('gtest_main', required: false, fallback: ['gtest', 'gtest_main_dep'])
gmock_dep      = dependency('gmock',      required: false, fallback: ['gtest', 'gmock_dep'])

if not gtest_dep.found()
    warning('GoogleTest not available — skipping tests')
    subdir_done()
endif

_inc  = include_directories('../include', 'fixtures', '.')
# wayland-client and wayland-server are needed by tests that include the
# framework headers (proxy.hpp, registry.hpp, resource_impl.hpp).
_deps = [
    scanner_lib_dep,
    gtest_dep,
    gmock_dep,
    wayland_client_dep,
    wayland_server_dep,
]

foreach suite : [
    ['raii',              'unit/test_raii.cpp',                          15],
    ['ir',                'unit/test_ir.cpp',                            15],
    ['xml_parser',        'unit/test_xml_parser.cpp',                    30],
    ['name_transform',    'unit/test_name_transform.cpp',                15],
    ['codegen_c',         'unit/test_codegen_c.cpp',                     30],
    ['codegen_client',    'unit/test_codegen_client_cxx.cpp',            30],
    ['codegen_server',    'unit/test_codegen_server_cxx.cpp',            30],
    ['codegen_interface_tables', 'unit/test_codegen_interface_tables.cpp', 30],
    ['event_map',         'unit/test_event_map.cpp',                     15],
    ['proxy',             'unit/test_proxy.cpp',                         15],
    ['resource_impl',     'unit/test_resource_impl.cpp',                 15],
    ['proxy_impl',        'unit/test_proxy_impl.cpp',                    15],
    ['client_helpers',    'unit/test_client_helpers.cpp',                15],
    ['display',           'unit/test_display.cpp',                       15],
    ['cli',               'unit/test_cli.cpp',                           15],
    ['roundtrip_client',  'integration/test_roundtrip_client.cpp',       60],
    ['roundtrip_server',  'integration/test_roundtrip_server.cpp',       60],
]
    _exe = executable(
        'test_' + suite[0], suite[1],
        include_directories: _inc,
        dependencies: _deps + [gtest_main_dep],
    )
    test(suite[0], _exe,
        args: [
            '--gtest_output=xml:results/test_' + suite[0] + '.xml',
            '--gtest_color=yes',
        ],
        timeout: suite[2],
        suite: 'unit',
        env: ['SRCDIR=' + meson.project_source_root()],
    )
endforeach

# ── Protocol handler tests (require generated headers) ───────────────────────
# These tests include generated C++ client headers (produced from XML protocol
# files by the wayland-cxx-scanner tool at build time) to test the generic
# CRTP handler templates in xdg_shell.hpp, seat.hpp, keyboard.hpp, and
# agl_shell.hpp.

# Helper: register a protocol handler test.
# @param name          Test target name (without 'test_' prefix).
# @param src           Source file path relative to tests/.
# @param headers       List of custom_target objects for generated headers.
# @param extra_deps    Extra dependencies beyond _deps (e.g. xkbcommon).
# @param timeout_s     Test timeout in seconds.

# ── agl_shell handler test ───────────────────────────────────────────────────
# agl-shell.xml is bundled in protocols/ so this test has no external deps.
agl_shell_client_hdr = custom_target(
    'test-agl-shell-client-header',
    input:   meson.project_source_root() / 'protocols' / 'agl-shell.xml',
    output:  'agl_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

_test_agl_shell_exe = executable(
    'test_agl_shell',
    ['unit/test_agl_shell.cpp', agl_shell_client_hdr],
    include_directories: _inc,
    dependencies: _deps + [gtest_main_dep],
)
test('agl_shell', _test_agl_shell_exe,
    args: [
        '--gtest_output=xml:results/test_agl_shell.xml',
        '--gtest_color=yes',
    ],
    timeout: 15,
    suite: 'unit',
    env: ['SRCDIR=' + meson.project_source_root()],
)

# ── xdg_shell handler test ───────────────────────────────────────────────────
# Requires wayland-protocols for xdg-shell.xml.
_wp_dep        = dependency('wayland-protocols', required: false)
_wl_scanner_dep = dependency('wayland-scanner',  required: false)

if _wp_dep.found() and _wl_scanner_dep.found()
    _xdg_shell_xml = _wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

    xdg_shell_client_hdr = custom_target(
        'test-xdg-shell-client-header',
        input:   _xdg_shell_xml,
        output:  'xdg_shell_client.hpp',
        command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
    )

    _test_xdg_shell_exe = executable(
        'test_xdg_shell',
        ['unit/test_xdg_shell.cpp', xdg_shell_client_hdr],
        include_directories: _inc,
        dependencies: _deps + [gtest_main_dep],
    )
    test('xdg_shell', _test_xdg_shell_exe,
        args: [
            '--gtest_output=xml:results/test_xdg_shell.xml',
            '--gtest_color=yes',
        ],
        timeout: 15,
        suite: 'unit',
        env: ['SRCDIR=' + meson.project_source_root()],
    )
else
    warning('wayland-protocols or wayland-scanner not found — skipping test_xdg_shell')
endif

# ── interface-table emission test ────────────────────────────────────────────
# Generates a client header with inline wl_interface tables (--emit-interface-
# tables) from a fixture and validates the tables at runtime, including pointer
# identity with the libwayland wl_surface_interface external reference.
if wayland_client_dep.found()
    iface_test_client_hdr = custom_target(
        'test-iface-test-client-header',
        input:   'fixtures/minimal_interfaces.xml',
        output:  'iface_test_client.hpp',
        command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
                  '@INPUT@', '@OUTPUT@'],
    )

    _test_iface_tables_exe = executable(
        'test_interface_tables',
        ['integration/test_interface_tables.cpp', iface_test_client_hdr],
        include_directories: _inc,
        dependencies: _deps + [gtest_main_dep],
    )
    test('interface_tables', _test_iface_tables_exe,
        args: [
            '--gtest_output=xml:results/test_interface_tables.xml',
            '--gtest_color=yes',
        ],
        timeout: 30,
        suite: 'unit',
        env: ['SRCDIR=' + meson.project_source_root()],
    )
else
    warning('wayland-client not found — skipping test_interface_tables')
endif

# ── IME facade contract test (ime_backend=none) ──────────────────────────────
# Includes backend.hpp, which only resolves cleanly without a selected backend.
if ime_backend == 'none'
    _test_ime_facades_exe = executable(
        'test_ime_facades',
        'unit/test_ime_facades.cpp',
        include_directories: _inc,
        dependencies: _deps + [gtest_main_dep],
    )
    test('ime_facades', _test_ime_facades_exe,
        args: [
            '--gtest_output=xml:results/test_ime_facades.xml',
            '--gtest_color=yes',
        ],
        timeout: 15,
        suite: 'unit',
        env: ['SRCDIR=' + meson.project_source_root()],
    )
endif

# ── text-input-v1 backend test (ime_backend=text-input-v1) ────────────────────
# Generates the v1 client header with inline interface tables and compiles the
# backend (via backend.hpp) against the ITextInputReceiver facade.
_ti_wp_dep = dependency('wayland-protocols', required: false)
if ime_backend == 'text-input-v1' and _ti_wp_dep.found() and wayland_client_dep.found()
    _ti_v1_xml = _ti_wp_dep.get_variable('pkgdatadir') / 'unstable/text-input/text-input-unstable-v1.xml'
    text_input_v1_client_hdr = custom_target(
        'test-text-input-v1-client-header',
        input:   _ti_v1_xml,
        output:  'text_input_v1_client.hpp',
        command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
                  '@INPUT@', '@OUTPUT@'],
    )

    _test_ti_v1_exe = executable(
        'test_text_input_v1_backend',
        ['integration/test_text_input_v1_backend.cpp', text_input_v1_client_hdr],
        include_directories: _inc,
        dependencies: _deps + [gtest_main_dep],
    )
    test('text_input_v1_backend', _test_ti_v1_exe,
        args: [
            '--gtest_output=xml:results/test_text_input_v1_backend.xml',
            '--gtest_color=yes',
        ],
        timeout: 30,
        suite: 'unit',
        env: ['SRCDIR=' + meson.project_source_root()],
    )
endif

# ── dispatch argument-name collision test ────────────────────────────────────
# Generates client + server headers from a fixture whose argument names collide
# with the dispatch callbacks' fixed parameters; the test only needs to compile.
if wayland_client_dep.found() and wayland_server_dep.found()
    collide_client_hdr = custom_target(
        'test-collide-client-header',
        input:   'fixtures/minimal_arg_collision.xml',
        output:  'collide_test_client.hpp',
        command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
    )
    collide_server_hdr = custom_target(
        'test-collide-server-header',
        input:   'fixtures/minimal_arg_collision.xml',
        output:  'collide_test_server.hpp',
        command: [scanner_exe, '--mode=server-header', '@INPUT@', '@OUTPUT@'],
    )

    _test_arg_collision_exe = executable(
        'test_arg_collision',
        ['integration/test_arg_collision.cpp', collide_client_hdr,
         collide_server_hdr],
        include_directories: _inc,
        dependencies: _deps + [gtest_main_dep],
    )
    test('arg_collision', _test_arg_collision_exe,
        args: [
            '--gtest_output=xml:results/test_arg_collision.xml',
            '--gtest_color=yes',
        ],
        timeout: 30,
        suite: 'unit',
        env: ['SRCDIR=' + meson.project_source_root()],
    )
else
    warning('wayland-client/server not found — skipping test_arg_collision')
endif

# ── seat/keyboard handler test ───────────────────────────────────────────────
# Requires wayland-scanner (for wayland.xml) and xkbcommon.
_xkbcommon_dep = dependency('xkbcommon', required: false)

if _wl_scanner_dep.found() and _xkbcommon_dep.found()
    _wayland_xml = _wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'

    wayland_client_hdr = custom_target(
        'test-wayland-client-header',
        input:   _wayland_xml,
        output:  'wayland_client.hpp',
        command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
    )

    _test_seat_exe = executable(
        'test_seat',
        ['unit/test_seat.cpp', wayland_client_hdr],
        include_directories: _inc,
        dependencies: _deps + [gtest_main_dep, _xkbcommon_dep],
    )
    test('seat', _test_seat_exe,
        args: [
            '--gtest_output=xml:results/test_seat.xml',
            '--gtest_color=yes',
        ],
        timeout: 15,
        suite: 'unit',
        env: ['SRCDIR=' + meson.project_source_root()],
    )
else
    warning('wayland-scanner or xkbcommon not found — skipping test_seat')
endif
