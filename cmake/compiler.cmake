# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Warning surface for first-party targets. Mirrors the Meson
# `warning_level=3 werror=true` defaults (see meson.build); -Werror is gated
# behind WAYLAND_CXX_WERROR so subproject consumers can relax it.
#
# First-party compiled targets link this PRIVATELY:
#   target_link_libraries(<tgt> PRIVATE wayland-cxx-scanner-warnings)
# It carries no transitive flags — nothing here propagates to consumers.

add_library(wayland-cxx-scanner-warnings INTERFACE)
add_library(wayland-cxx-scanner::warnings ALIAS wayland-cxx-scanner-warnings)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wayland-cxx-scanner-warnings INTERFACE
        -Wall -Wextra -Wpedantic)
    if (WAYLAND_CXX_WERROR)
        target_compile_options(wayland-cxx-scanner-warnings INTERFACE -Werror)
    endif ()
elseif (MSVC)
    target_compile_options(wayland-cxx-scanner-warnings INTERFACE /W4)
    if (WAYLAND_CXX_WERROR)
        target_compile_options(wayland-cxx-scanner-warnings INTERFACE /WX)
    endif ()
endif ()
