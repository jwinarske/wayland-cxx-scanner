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

<<<<<<< Updated upstream
=======
# The scanner is a codegen tool run *during a consumer's build* to generate
# headers. The freshly-built binary must therefore start in that build's
# environment — including consumers whose toolchain keeps the C++ runtime
# outside the system loader path (e.g. flutter_workspace's clang, where
# libc++abi.so.1 lives in the toolchain lib dir, not /usr/lib). A dynamically
# linked tool fails there with "libc++abi.so.1: cannot open shared object
# file" mid-build, which in turn leaves the generated headers stale/absent and
# surfaces as unrelated-looking compile errors downstream. Static-link the C++
# runtime so the tool is self-contained. (-static-libgcc is deliberately NOT
# added: a standalone tool never unwinds across a shared boundary, and mixing
# it in has bitten libc++ exception handling elsewhere.)
#
# Gate on an actual link probe, not just the compiler id: some distros ship
# only the shared C++ runtime (no static archive), where -static-libstdc++
# fails to link. Those builds fall back to dynamic — which is fine, since the
# missing-static-archive case is exactly the case where the runtime is on the
# system loader path anyway. (Mirrors the Meson side's has_link_argument gate.)
include(CheckCXXSourceCompiles)
set(_wlcxx_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -static-libstdc++")
check_cxx_source_compiles("int main() { return 0; }"
    WLCXX_SCANNER_STATIC_LIBSTDCXX)
set(CMAKE_REQUIRED_FLAGS "${_wlcxx_saved_required_flags}")
if (WLCXX_SCANNER_STATIC_LIBSTDCXX)
    target_link_options(wayland-cxx-scanner PRIVATE -static-libstdc++)
endif ()

>>>>>>> Stashed changes
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
