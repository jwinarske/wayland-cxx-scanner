# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# CMake build for wayland-cxx-scanner. Maintained in parallel with the Meson
# build (the source of truth) — see docs/cmake_plan.md.
#
# Implemented so far: P0 (conventions) + P1 (core build + scanner exe).
# Pending: P2 (header-only framework + IME option), P3 (wayland_cxx_generate()
# + cross-compile host-tool resolution), P4 (install/export), P5/P6 (tests/
# examples). Those slot in where marked below.

# Floor matches the consuming project's declared minimum so this submodule never
# raises the consumer's required CMake version. Everything used here (include_guard,
# cxx_std_17 compile features, PARSE_ARGV, package-config helpers, pkg-config
# imported targets) is available at 3.10.2.
cmake_minimum_required(VERSION 3.10.2)

project(wayland-cxx-scanner
    VERSION 0.1.0
    DESCRIPTION "WTL-patterned C++17 Wayland binding generator and framework"
    LANGUAGES CXX)

# House conventions ------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)

# Default C++ standard (Meson `cpp_std=c++17`). No compiler extensions.
if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif ()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Honor a parent project's BUILD_SHARED_LIBS; default static like the Meson wrap.
if (NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF)
endif ()

include(options)
include(compiler)

# Enable CTest at the top level so tests registered in tests/ AND examples/
# (the minimal roundtrip) aggregate into the root test set.
enable_testing()

# Dependencies -----------------------------------------------------------------

# pugixml — system dependency (the distro's -devel/-dev package: Fedora
# pugixml-devel, Debian libpugixml-dev, Arch/Alpine pugixml, Yocto meta-oe
# pugixml). Prefer the CMake config package; fall back to pkg-config, the
# mechanism the Meson build already uses. No bundled or downloaded copy — a
# missing dev package fails fast at configure. WAYLAND_CXX_PUGIXML_TARGET names
# whichever imported target was found, for src/ to link.
#
# Only required when THIS project builds the scanner (native always, or cross
# with BUILD_TOOL=ON — exactly WAYLAND_CXX_SCANNER_BUILD_TOOL). A cross examples
# build consumes a host scanner via find_program and never compiles pugixml, so
# it must not be required there.
if (WAYLAND_CXX_SCANNER_BUILD_TOOL)
    find_package(pugixml CONFIG QUIET)
    if (pugixml_FOUND)
        set(WAYLAND_CXX_PUGIXML_TARGET pugixml::pugixml)
    else ()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(pugixml REQUIRED IMPORTED_TARGET pugixml)
        set(WAYLAND_CXX_PUGIXML_TARGET PkgConfig::pugixml)
    endif ()
endif ()

# Header-only framework --------------------------------------------------------
#
# Pure include dirs — nothing is compiled, so this target exists in every build
# mode. Replaces the *include* role of waypp's wayland-gen and the *header*
# surface of waypp. Carries the IME backend define as INTERFACE so consumers
# (e.g. ivi) inherit the exact same single-backend choice resolved above.
add_library(wayland-cxx INTERFACE)
add_library(wayland-cxx::wayland-cxx ALIAS wayland-cxx)

target_compile_features(wayland-cxx INTERFACE cxx_std_17)
target_include_directories(wayland-cxx INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(wayland-cxx INTERFACE ${WAYLAND_CXX_IME_DEFINE})

message(STATUS "wayland-cxx: IME backend '${WAYLAND_CXX_IME_BACKEND}' "
               "(-D${WAYLAND_CXX_IME_DEFINE})")

# Scanner resolution + codegen API (§4 / §5) -----------------------------------
#
# A single branch on CMAKE_CROSSCOMPILING resolves the build-time codegen tool.
# Variables are cached (CACHE INTERNAL) so external add_subdirectory consumers
# (e.g. ivi) see them when they call wayland_cxx_generate() in their own scope.

# Build-order dep for codegen; empty under cross so generation orders against the
# XML only, never against an unrunnable target-arch exe (BUILD_TOOL=ON).
set(WAYLAND_CXX_SCANNER_CODEGEN_TARGET "" CACHE INTERNAL "codegen ordering target")

# The public wayland_cxx_generate() API (function is global once included).
# Included BEFORE the branch so the tests/examples subdirectories can call it.
include(WaylandCxxScanner)

if (CMAKE_CROSSCOMPILING)
    if (WAYLAND_CXX_SCANNER_TOOL_ONLY)
        message(FATAL_ERROR
            "TOOL_ONLY is meaningless when cross-compiling: build the scanner "
            "natively and consume it via PATH or -DWAYLAND_CXX_SCANNER_EXECUTABLE.")
    endif ()

    if (WAYLAND_CXX_SCANNER_BUILD_TOOL)
        # Target-arch scanner — ON-DEVICE deliverable ONLY. Intentionally NOT
        # assigned to *_EXECUTABLE / *_CODEGEN_TARGET: it can't run on the build
        # host, so it must never drive codegen.
        add_subdirectory(src)
    endif ()

    if (WAYLAND_CXX_SCANNER_BUILD_EXAMPLES)
        # Host tool: Yocto's wayland-cxx-scanner-native on PATH, or pass a path
        # via -DWAYLAND_CXX_SCANNER_EXECUTABLE. REQUIRED → fails fast at configure.
        find_program(WAYLAND_CXX_SCANNER_EXECUTABLE NAMES wayland-cxx-scanner REQUIRED)
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
            add_subdirectory(examples)            # built for the target
        else ()
            message(WARNING "BUILD_EXAMPLES=ON but examples/CMakeLists.txt absent (P6)")
        endif ()
    endif ()
    # tests are host-run → skipped in cross
else ()
    add_subdirectory(src)                         # host scanner = the codegen tool
    set(WAYLAND_CXX_SCANNER_EXECUTABLE "$<TARGET_FILE:wayland-cxx-scanner>"
        CACHE INTERNAL "wayland-cxx-scanner codegen tool (host)")
    set(WAYLAND_CXX_SCANNER_CODEGEN_TARGET wayland-cxx-scanner
        CACHE INTERNAL "codegen ordering target")

    if (NOT WAYLAND_CXX_SCANNER_TOOL_ONLY)
        if (WAYLAND_CXX_SCANNER_BUILD_EXAMPLES)
            if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
                add_subdirectory(examples)        # host, vs the just-built scanner
            else ()
                message(WARNING "BUILD_EXAMPLES=ON but examples/CMakeLists.txt absent (P6)")
            endif ()
        endif ()
        if (WAYLAND_CXX_SCANNER_BUILD_TESTS)
            if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
                add_subdirectory(tests)
            else ()
                message(WARNING "BUILD_TESTS=ON but tests/CMakeLists.txt absent (P5)")
            endif ()
        endif ()
    endif ()
endif ()

# Install / export (§6 P4) -----------------------------------------------------
#
# TOOL_ONLY ships only the exe (handled in src/): skip framework headers, the
# CMake package, and the .pc.
if (NOT WAYLAND_CXX_SCANNER_TOOL_ONLY)
    include(CMakePackageConfigHelpers)

    set(_wlcxx_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/wayland-cxx-scanner")

    # Framework headers — MIRROR meson.build install_headers EXACTLY (lines
    # 45-84). Do NOT glob include/wl: it carries headers Meson does not install
    # (e.g. agl_shell.hpp). Keep in lock-step with Meson (§8).
    install(FILES
        include/wl/client_helpers.hpp
        include/wl/csd_cairo.hpp
        include/wl/csd_common.hpp
        include/wl/csd_fallback.hpp
        include/wl/csd_gtk.hpp
        include/wl/csd_plugin.hpp
        include/wl/data_device.hpp
        include/wl/display.hpp
        include/wl/fd_handle.hpp
        include/wl/linux_dmabuf.hpp
        include/wl/raii.hpp
        include/wl/event_map.hpp
        include/wl/span.hpp
        include/wl/proxy.hpp
        include/wl/proxy_impl.hpp
        include/wl/wl_ptr.hpp
        include/wl/resource_impl.hpp
        include/wl/registry.hpp
        include/wl/wayland.hpp
        include/wl/xdg_shell.hpp
        include/wl/xdg_decoration.hpp
        include/wl/keyboard.hpp
        include/wl/pointer.hpp
        include/wl/present_feedback.hpp
        include/wl/presentation.hpp
        include/wl/seat.hpp
        include/wl/touch.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wl)
    install(FILES
        include/wl/ime/text_input_receiver.hpp
        include/wl/ime/input_method.hpp
        include/wl/ime/virtual_keyboard.hpp
        include/wl/ime/backend.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wl/ime)
    install(FILES
        include/wl/ime/backends/text_input_v1.hpp
        include/wl/ime/backends/text_input_v3.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wl/ime/backends)

    # Header-only framework target into the package export set.
    install(TARGETS wayland-cxx EXPORT wayland-cxx-scanner-targets)

    # Export set (framework + the scanner exe tagged in src/, when installed).
    install(EXPORT wayland-cxx-scanner-targets
        FILE      wayland-cxx-scanner-targets.cmake
        NAMESPACE wayland-cxx::
        DESTINATION "${_wlcxx_cmake_dir}")

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wayland-cxx-scannerConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/wayland-cxx-scannerConfig.cmake
        INSTALL_DESTINATION "${_wlcxx_cmake_dir}")
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/wayland-cxx-scannerConfigVersion.cmake
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    # Ship the config, version file, and the public codegen function module.
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/wayland-cxx-scannerConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/wayland-cxx-scannerConfigVersion.cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WaylandCxxScanner.cmake
        DESTINATION "${_wlcxx_cmake_dir}")

    # pkg-config — same wayland-cxx.pc the Meson build installs (§8 parity).
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wayland-cxx.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/wayland-cxx.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wayland-cxx.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif ()

# P5/P6 — tests / examples, gated on WAYLAND_CXX_SCANNER_BUILD_{TESTS,EXAMPLES}.
