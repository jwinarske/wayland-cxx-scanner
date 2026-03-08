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
_deps = [scanner_lib_dep, gtest_dep, gmock_dep]

foreach suite : [
    ['raii',              'unit/test_raii.cpp',                          15],
    ['ir',                'unit/test_ir.cpp',                            15],
    ['xml_parser',        'unit/test_xml_parser.cpp',                    30],
    ['name_transform',    'unit/test_name_transform.cpp',                15],
    ['codegen_c',         'unit/test_codegen_c.cpp',                     30],
    ['codegen_client',    'unit/test_codegen_client_cxx.cpp',            30],
    ['codegen_server',    'unit/test_codegen_server_cxx.cpp',            30],
    ['event_map',         'unit/test_event_map.cpp',                     15],
    ['proxy',             'unit/test_proxy.cpp',                         15],
    ['resource_impl',     'unit/test_resource_impl.cpp',                 15],
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
