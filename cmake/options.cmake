# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Centralized build options. Mirrors meson.options; keep the two in lock-step
# (see docs/cmake_plan.md §8). Each option notes the phase that wires it up.

# --- P1: scanner tool ---------------------------------------------------------

# Build only the scanner executable (+ pugixml): skip framework install, tests,
# examples, docs. Valid only in native builds (see §4). Consumed by the Yocto
# `-native` recipe when the tool is built standalone.
option(WAYLAND_CXX_SCANNER_TOOL_ONLY "Build only the scanner executable" OFF)

# Treat warnings as errors (Meson `werror=true`). Off by consumers (e.g. ivi)
# that vendor this as a subproject and relax warnings.
option(WAYLAND_CXX_WERROR "Treat compiler warnings as errors" ON)

# Build the scanner exe as an output of THIS project (see §4):
#   native: always ON — it is also the in-tree codegen tool.
#   cross:  OFF by default — ON only to ship an ON-DEVICE runtime scanner; that
#           target-arch binary is never assigned as the codegen tool.
if (CMAKE_CROSSCOMPILING)
    option(WAYLAND_CXX_SCANNER_BUILD_TOOL
        "Build target-arch scanner for on-device use" OFF)
else ()
    set(WAYLAND_CXX_SCANNER_BUILD_TOOL ON CACHE INTERNAL "")
endif ()

# --- P2: header-only framework + IME backend ----------------------------------

# IME protocol backend to generate/wrap/build (exactly one). Mirrors the Meson
# `ime_backend` combo. NOTE: the backend is a *compositor capability* owned by
# the compositor/BSP layer (see docs/cmake_plan.md §P2/§7), not really a project
# default — this value is the last-resort fallback, overridable by consumers.
set(WAYLAND_CXX_IME_BACKEND "text-input-v1" CACHE STRING
    "IME protocol backend to build (exactly one)")
set(_wlcxx_ime_choices
    none text-input-v1 text-input-v3
    input-method-v1 input-method-v2 virtual-keyboard-v1)
set_property(CACHE WAYLAND_CXX_IME_BACKEND PROPERTY STRINGS ${_wlcxx_ime_choices})

if (NOT WAYLAND_CXX_IME_BACKEND IN_LIST _wlcxx_ime_choices)
    string(REPLACE ";" ", " _wlcxx_ime_list "${_wlcxx_ime_choices}")
    message(FATAL_ERROR
        "WAYLAND_CXX_IME_BACKEND='${WAYLAND_CXX_IME_BACKEND}' is not one of: "
        "${_wlcxx_ime_list}")
endif ()

# Map backend -> the compile define consumed by include/wl/ime/backend.hpp
# (e.g. text-input-v1 -> WL_IME_BACKEND_TEXT_INPUT_V1).
string(TOUPPER "${WAYLAND_CXX_IME_BACKEND}" _wlcxx_ime_token)
string(REPLACE "-" "_" _wlcxx_ime_token "${_wlcxx_ime_token}")
set(WAYLAND_CXX_IME_DEFINE "WL_IME_BACKEND_${_wlcxx_ime_token}")
unset(_wlcxx_ime_choices)
unset(_wlcxx_ime_token)

# --- P4: install / export (declared now, wired later) -------------------------

# Gate whether the scanner executable is installed at all, so a consumer that
# only wants the header-only framework + generated headers can drop the binary.
option(WAYLAND_CXX_SCANNER_INSTALL_TOOL "Install the scanner executable" ON)

# --- P5/P6: tests & examples (declared now, wired later) ----------------------

option(WAYLAND_CXX_SCANNER_BUILD_TESTS    "Build unit/integration tests" OFF)
option(WAYLAND_CXX_SCANNER_BUILD_EXAMPLES "Build example applications"   OFF)

# Build the test tier under AddressSanitizer + UndefinedBehaviorSanitizer, so
# CTest can run the socket-pair roundtrip integration tier under sanitizers.
option(WAYLAND_CXX_SCANNER_SANITIZE "Build tests with ASan + UBSan" OFF)
