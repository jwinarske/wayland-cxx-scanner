# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

scanner_lib_sources = files(
    'xml_parser.cpp',
    'name_transform.cpp',
    'codegen_c.cpp',
    'codegen_client_cxx.cpp',
    'codegen_server_cxx.cpp',
    'codegen_interface_tables.cpp',
)

scanner_lib = static_library(
    'wayland-cxx-scanner-lib',
    scanner_lib_sources,
    include_directories: include_directories('.'),
    dependencies: [pugixml_dep],
    cpp_args: ['-DWLSCANNER_BUILD'],
)

scanner_lib_dep = declare_dependency(
    link_with: scanner_lib,
    include_directories: include_directories('.'),
    dependencies: [pugixml_dep],
)

# Static-link the C++ runtime so the codegen tool is self-contained and runs
# during any consumer's build, including toolchains that keep libc++abi outside
# the system loader path. See src/CMakeLists.txt for the full rationale.
_scanner_cxx = meson.get_compiler('cpp')
_scanner_link_args = []
if (_scanner_cxx.get_id() in ['gcc', 'clang']
        and _scanner_cxx.has_link_argument('-static-libstdc++'))
    _scanner_link_args += '-static-libstdc++'
endif

scanner_exe = executable(
    'wayland-cxx-scanner',
    'main.cpp',
    dependencies: [scanner_lib_dep],
    link_args: _scanner_link_args,
    install: get_option('install_tool'),
)
