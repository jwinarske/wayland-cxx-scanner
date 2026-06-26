# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Scanner static library + executable. Mirrors src/meson.build.

# Internal static library: the codegen/parse/transform TUs. Not exported
# (consumed by the exe and, later, the scanner's own tests — see §5).
add_library(wayland-cxx-scanner-lib STATIC
    xml_parser.cpp
    name_transform.cpp
    codegen_c.cpp
    codegen_client_cxx.cpp
    codegen_server_cxx.cpp
    codegen_interface_tables.cpp)

target_include_directories(wayland-cxx-scanner-lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(wayland-cxx-scanner-lib PRIVATE WLSCANNER_BUILD)

target_link_libraries(wayland-cxx-scanner-lib
    PUBLIC  ${WAYLAND_CXX_PUGIXML_TARGET}
    PRIVATE wayland-cxx-scanner-warnings)

# The scanner executable — also the in-tree codegen tool in native builds.
add_executable(wayland-cxx-scanner main.cpp)
target_link_libraries(wayland-cxx-scanner
    PRIVATE wayland-cxx-scanner-lib wayland-cxx-scanner-warnings)

# Install gating. Under cross the exe only exists when BUILD_TOOL=ON;
# INSTALL_TOOL drops the binary regardless. TOOL_ONLY ships a bare binary with
# no CMake package, so it is installed without EXPORT membership; otherwise the
# exe joins the package export set so find_package consumers get an IMPORTED
# wayland-cxx::wayland-cxx-scanner.
if (TARGET wayland-cxx-scanner AND WAYLAND_CXX_SCANNER_INSTALL_TOOL)
    if (WAYLAND_CXX_SCANNER_TOOL_ONLY)
        install(TARGETS wayland-cxx-scanner
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    else ()
        install(TARGETS wayland-cxx-scanner
            EXPORT  wayland-cxx-scanner-targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif ()
endif ()
