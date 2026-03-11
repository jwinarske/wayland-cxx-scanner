# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

scanner_lib_sources = files(
    'xml_parser.cpp',
    'name_transform.cpp',
    'codegen_c.cpp',
    'codegen_client_cxx.cpp',
    'codegen_server_cxx.cpp',
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

scanner_exe = executable(
    'wayland-cxx-scanner',
    'main.cpp',
    dependencies: [scanner_lib_dep],
    install: true,
)
