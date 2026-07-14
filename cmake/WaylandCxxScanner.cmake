# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Public code-gen API. Installed (P4) and also included by add_subdirectory
# consumers, so both consumption modes get the same function.
#
# wayland_cxx_generate(
#   PROTOCOL <xml>
#   MODE     client-header | server-header | c-header
#   OUTPUT   <out.hpp>                 # relative to ${CMAKE_CURRENT_BINARY_DIR}
#   [STD     c++17|c++20|c++23]        # default: the scanner's own default
#   [EMIT_INTERFACE_TABLES]            # extension protocols (no libwayland symbol)
#   [TARGET  <tgt>]                    # attach generated-header dep + include dir
#   [OUTPUT_VAR <var>]                 # append generated path to <var> (list)
# )
#
# Resolution contract (see docs/cmake_plan.md §4):
#   - WAYLAND_CXX_SCANNER_EXECUTABLE      the codegen tool to invoke.
#   - WAYLAND_CXX_SCANNER_CODEGEN_TARGET  build-order dep, set ONLY in native
#     builds. Empty under cross, so codegen orders against the XML alone and
#     never against an unrunnable target-arch scanner (BUILD_TOOL=ON).

include_guard(GLOBAL)

function(wayland_cxx_generate)
    cmake_parse_arguments(PARSE_ARGV 0 _g
        "EMIT_INTERFACE_TABLES"                       # options
        "PROTOCOL;MODE;OUTPUT;STD;TARGET;OUTPUT_VAR"  # one-value
        "")                                           # multi-value
    if (NOT _g_PROTOCOL OR NOT _g_MODE OR NOT _g_OUTPUT)
        message(FATAL_ERROR "wayland_cxx_generate: PROTOCOL, MODE, OUTPUT are required")
    endif ()
    if (NOT WAYLAND_CXX_SCANNER_EXECUTABLE)
        message(FATAL_ERROR "wayland_cxx_generate: WAYLAND_CXX_SCANNER_EXECUTABLE unset")
    endif ()

    get_filename_component(_protocol "${_g_PROTOCOL}" ABSOLUTE)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_g_OUTPUT}")
    get_filename_component(_out_dir "${_out}" DIRECTORY)
    file(MAKE_DIRECTORY "${_out_dir}")

    # Native: order codegen after the in-tree exe. Cross: codegen target is empty
    # so we depend on the XML only — never on an unrunnable target-arch exe (which
    # may exist when BUILD_TOOL=ON). Keyed off the explicit var, not TARGET exists.
    set(_scanner_dep "${WAYLAND_CXX_SCANNER_CODEGEN_TARGET}")

    # Optional flags are configure-time known, so assemble them with if() rather
    # than $<...> genexes — an empty genex would pass a literal "" arg the
    # scanner rejects as an empty positional.
    set(_flags "")
    if (_g_STD)
        list(APPEND _flags "--std=${_g_STD}")
    endif ()
    if (_g_EMIT_INTERFACE_TABLES)
        list(APPEND _flags "--emit-interface-tables")
    endif ()

    # Two targets can legitimately consume the same generated header — the seat
    # and pointer-axis tests both include wayland_client.hpp — so emit the rule
    # once and attach every caller to it.  Repeating add_custom_command for one
    # OUTPUT is an error, and the generating target's name is global.  A second
    # request for the same output that does not match the first is a mistake
    # rather than a share, and says so instead of silently keeping one of them.
    string(MAKE_C_IDENTIFIER "wlcxxgen_${_g_OUTPUT}" _gen_tgt)
    set(_spec "${_out}|${_g_MODE}|${_flags}|${_protocol}")
    get_property(_prev GLOBAL PROPERTY ${_gen_tgt}_spec)
    if (_prev)
        if (NOT "${_prev}" STREQUAL "${_spec}")
            message(FATAL_ERROR
                "wayland_cxx_generate: ${_g_OUTPUT} was already requested as\n"
                "    ${_prev}\n"
                "  and cannot also be generated as\n"
                "    ${_spec}")
        endif ()
    else ()
        set_property(GLOBAL PROPERTY ${_gen_tgt}_spec "${_spec}")
        add_custom_command(
            OUTPUT  "${_out}"
            COMMAND ${WAYLAND_CXX_SCANNER_EXECUTABLE}
                    --mode=${_g_MODE}
                    ${_flags}
                    "${_protocol}" "${_out}"
            DEPENDS "${_protocol}" ${_scanner_dep}
            COMMENT "Generating ${_g_OUTPUT} (${_g_MODE}) from ${_g_PROTOCOL}"
            VERBATIM)
        add_custom_target(${_gen_tgt} DEPENDS "${_out}")
    endif ()

    if (_g_TARGET)
        # Attach the generated header to the consumer target: dep ordering +
        # the binary output dir on the include path.
        add_dependencies(${_g_TARGET} ${_gen_tgt})
        target_include_directories(${_g_TARGET} PRIVATE "${_out_dir}")
    endif ()
    if (_g_OUTPUT_VAR)
        set(${_g_OUTPUT_VAR} "${${_g_OUTPUT_VAR}};${_out}" PARENT_SCOPE)
    endif ()
endfunction()
