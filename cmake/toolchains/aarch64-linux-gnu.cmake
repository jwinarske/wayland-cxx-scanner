# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Cross-compile toolchain: arm64 (aarch64) Linux, GNU toolchain. Used by the
# CI cross job to prove the §4 host-tool story — examples are built FOR the
# target while the codegen scanner is resolved from the BUILD HOST.
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
#         -DWAYLAND_CXX_SCANNER_BUILD_EXAMPLES=ON
#
# Assumes a multiarch host: g++-aarch64-linux-gnu plus the target -dev libs
# (libwayland-dev:arm64, libxkbcommon-dev:arm64, …).

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_triple aarch64-linux-gnu)
set(CMAKE_C_COMPILER   ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)

# Probe the compiler by building a static library, not a linked executable, so
# configure works on hosts that have the cross compiler but no linkable target
# runtime (e.g. a CI/dev box before the :arm64 libs are installed). Real
# example executables still link normally during the build.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Search target libraries/headers in the multiarch sysroot, but resolve
# PROGRAMS from the host PATH — this is what lets find_program() pick up the
# host-built wayland-cxx-scanner (NEVER a target-arch binary).
set(CMAKE_FIND_ROOT_PATH /usr/${_triple} /usr/lib/${_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Point pkg-config at the target-arch .pc files (multiarch layout). Arch-
# independent protocol data (wayland.xml, wayland-protocols) is also covered
# by /usr/share/pkgconfig.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${_triple}/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
