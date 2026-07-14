# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# CTest port of tests/meson.build. Mirrors the Meson suite list, per-test
# timeouts, and the SRCDIR env; keep the two in lock-step (§8). Protocol-handler
# tests generate their headers with the project's own wayland_cxx_generate(),
# which doubles as in-repo coverage of the public codegen API.

# GoogleTest — system package (config or FindGTest module). No bundled/fetched
# copy, matching the system-dependency policy used for pugixml.
find_package(GTest REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(wayland_client QUIET IMPORTED_TARGET wayland-client)
pkg_check_modules(wayland_server QUIET IMPORTED_TARGET wayland-server)
pkg_check_modules(xkbcommon      QUIET IMPORTED_TARGET xkbcommon)

# Protocol XML lives in the wayland-protocols / wayland-scanner data dirs.
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_SCANNER_DIR   wayland-scanner   pkgdatadir)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")

# wayland-client/server are linked into every test when available (the Meson
# build lists them in the common dep set); absent → the few tests that truly
# need them are guarded below.
set(_wlcxx_test_wayland "")
if (TARGET PkgConfig::wayland_client)
    list(APPEND _wlcxx_test_wayland PkgConfig::wayland_client)
endif ()
if (TARGET PkgConfig::wayland_server)
    list(APPEND _wlcxx_test_wayland PkgConfig::wayland_server)
endif ()

# wlcxx_add_test(<name> <timeout> <src>...) — gtest exe + CTest registration.
function(wlcxx_add_test name timeout)
    add_executable(test_${name} ${ARGN})
    target_include_directories(test_${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
    target_link_libraries(test_${name} PRIVATE
        wayland-cxx-scanner-lib        # scanner lib headers (ir.hpp, …) + pugixml
        wayland-cxx::wayland-cxx        # framework headers (include/wl/…)
        GTest::gtest GTest::gmock GTest::gtest_main
        ${_wlcxx_test_wayland})
    if (WAYLAND_CXX_SCANNER_SANITIZE)
        target_compile_options(test_${name} PRIVATE -fsanitize=address,undefined)
        # 3.10-safe sanitizer link wiring (target_link_options is 3.13+).
        target_link_libraries(test_${name} PRIVATE -fsanitize=address,undefined)
    endif ()
    add_test(NAME ${name}
        COMMAND test_${name}
            --gtest_output=xml:results/test_${name}.xml
            --gtest_color=yes)
    set_tests_properties(${name} PROPERTIES
        TIMEOUT     ${timeout}
        ENVIRONMENT "SRCDIR=${PROJECT_SOURCE_DIR}")
endfunction()

# ── Plain unit/integration suites (no generated headers) ─────────────────────
foreach(spec
    "raii|unit/test_raii.cpp|15"
    "ir|unit/test_ir.cpp|15"
    "xml_parser|unit/test_xml_parser.cpp|30"
    "name_transform|unit/test_name_transform.cpp|15"
    "codegen_c|unit/test_codegen_c.cpp|30"
    "codegen_client|unit/test_codegen_client_cxx.cpp|30"
    "codegen_server|unit/test_codegen_server_cxx.cpp|30"
    "codegen_interface_tables|unit/test_codegen_interface_tables.cpp|30"
    "event_map|unit/test_event_map.cpp|15"
    "present_feedback|unit/test_present_feedback.cpp|15"
    "scale_policy|unit/test_scale_policy.cpp|15"
    "proxy|unit/test_proxy.cpp|15"
    "resource_impl|unit/test_resource_impl.cpp|15"
    "proxy_impl|unit/test_proxy_impl.cpp|15"
    "client_helpers|unit/test_client_helpers.cpp|15"
    "display|unit/test_display.cpp|15"
    "cli|unit/test_cli.cpp|15"
    "roundtrip_client|integration/test_roundtrip_client.cpp|60"
    "roundtrip_server|integration/test_roundtrip_server.cpp|60")
    string(REPLACE "|" ";" _p "${spec}")
    list(GET _p 0 _name)
    list(GET _p 1 _src)
    list(GET _p 2 _to)
    wlcxx_add_test(${_name} ${_to} ${_src})
endforeach()

# ── Protocol-handler / generated-header tests ────────────────────────────────

# agl_shell — agl-shell.xml is bundled, so no external deps.  agl_shell is not
# in wayland-protocols and has no pre-built wl_interface symbol, so the header
# has to carry its own tables.
wlcxx_add_test(agl_shell 15 unit/test_agl_shell.cpp)
wayland_cxx_generate(
    PROTOCOL ${PROJECT_SOURCE_DIR}/protocols/agl-shell.xml
    MODE     client-header
    OUTPUT   agl_shell_client.hpp
    EMIT_INTERFACE_TABLES
    TARGET   test_agl_shell)

# xdg_shell — needs wayland-protocols (xdg-shell.xml).
if (WAYLAND_PROTOCOLS_DIR)
    wlcxx_add_test(xdg_shell 15 unit/test_xdg_shell.cpp)
    wayland_cxx_generate(
        PROTOCOL ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
        MODE     client-header
        OUTPUT   xdg_shell_client.hpp
        TARGET   test_xdg_shell)
else ()
    message(STATUS "wayland-protocols not found — skipping test_xdg_shell")
endif ()

# interface_tables — emits inline wl_interface tables from a fixture.
if (TARGET PkgConfig::wayland_client)
    wlcxx_add_test(interface_tables 30 integration/test_interface_tables.cpp)
    wayland_cxx_generate(
        PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimal_interfaces.xml
        MODE     client-header
        OUTPUT   iface_test_client.hpp
        EMIT_INTERFACE_TABLES
        TARGET   test_interface_tables)
else ()
    message(STATUS "wayland-client not found — skipping test_interface_tables")
endif ()

# ime_facades — only resolves cleanly with no backend selected.
if (WAYLAND_CXX_IME_BACKEND STREQUAL "none")
    wlcxx_add_test(ime_facades 15 unit/test_ime_facades.cpp)
endif ()

# text_input_backend — compiles the selected text-input backend against the
# ITextInputReceiver facade.
if (WAYLAND_CXX_IME_BACKEND MATCHES "^text-input-v[13]$"
    AND WAYLAND_PROTOCOLS_DIR AND TARGET PkgConfig::wayland_client)
    if (WAYLAND_CXX_IME_BACKEND STREQUAL "text-input-v3")
        set(_ti_xml ${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v3.xml)
        set(_ti_out text_input_v3_client.hpp)
    else ()
        set(_ti_xml ${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v1.xml)
        set(_ti_out text_input_v1_client.hpp)
    endif ()
    wlcxx_add_test(text_input_backend 30 integration/test_text_input_backend.cpp)
    wayland_cxx_generate(
        PROTOCOL ${_ti_xml}
        MODE     client-header
        OUTPUT   ${_ti_out}
        EMIT_INTERFACE_TABLES
        TARGET   test_text_input_backend)
endif ()

# arg_collision — generates client + server headers from a collision fixture;
# only needs to compile.
if (TARGET PkgConfig::wayland_client AND TARGET PkgConfig::wayland_server)
    wlcxx_add_test(arg_collision 30 integration/test_arg_collision.cpp)
    wayland_cxx_generate(
        PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimal_arg_collision.xml
        MODE     client-header
        OUTPUT   collide_test_client.hpp
        TARGET   test_arg_collision)
    wayland_cxx_generate(
        PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimal_arg_collision.xml
        MODE     server-header
        OUTPUT   collide_test_server.hpp
        TARGET   test_arg_collision)
else ()
    message(STATUS "wayland-client/server not found — skipping test_arg_collision")
endif ()

# seat/keyboard — needs wayland-scanner (wayland.xml) and xkbcommon.
if (WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_add_test(seat 15 unit/test_seat.cpp)
    wayland_cxx_generate(
        PROTOCOL ${WAYLAND_SCANNER_DIR}/wayland.xml
        MODE     client-header
        OUTPUT   wayland_client.hpp
        TARGET   test_seat)
    target_link_libraries(test_seat PRIVATE PkgConfig::xkbcommon)
else ()
    message(STATUS "wayland-scanner or xkbcommon not found — skipping test_seat")
endif ()

# ── Live-dispatch tier ───────────────────────────────────────────────────────
# Each of these drives libwayland across a socketpair so the generated code is
# invoked through the real C listener ABI.  It is the only tier that catches a
# wrong-width or misrouted argument slot, and the defects it catches are silent
# without a sanitizer, so it must exist in both builds rather than Meson alone.

if (TARGET PkgConfig::wayland_client AND TARGET PkgConfig::wayland_server)
    # event_new_id_roundtrip — new_id delivered through an event.
    wlcxx_add_test(event_new_id_roundtrip 60
        integration/test_event_new_id_roundtrip.cpp)
    wayland_cxx_generate(
        PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimal_new_id_event.xml
        MODE     client-header
        OUTPUT   thingy_client.hpp
        EMIT_INTERFACE_TABLES
        TARGET   test_event_new_id_roundtrip)

    # server_event_new_id_roundtrip — the same, in the server direction.  Only
    # the server header carries tables; the client bridges to them.
    wlcxx_add_test(server_event_new_id_roundtrip 60
        integration/test_server_event_new_id_roundtrip.cpp)
    wayland_cxx_generate(
        PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimal_new_id_event.xml
        MODE     server-header
        OUTPUT   srv_thingy_server.hpp
        EMIT_INTERFACE_TABLES
        TARGET   test_server_event_new_id_roundtrip)
    wayland_cxx_generate(
        PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/minimal_new_id_event.xml
        MODE     client-header
        OUTPUT   srv_thingy_client.hpp
        TARGET   test_server_event_new_id_roundtrip)
else ()
    message(STATUS
        "wayland-client/server not found — skipping the new_id roundtrip tests")
endif ()

# data_device_roundtrip — core wl_data_device selection over the wire.
if (WAYLAND_SCANNER_DIR
    AND TARGET PkgConfig::wayland_client AND TARGET PkgConfig::wayland_server)
    wlcxx_add_test(data_device_roundtrip 60
        integration/test_data_device_roundtrip.cpp)
    wayland_cxx_generate(
        PROTOCOL ${WAYLAND_SCANNER_DIR}/wayland.xml
        MODE     client-header
        OUTPUT   dd_client.hpp
        EMIT_INTERFACE_TABLES
        TARGET   test_data_device_roundtrip)
else ()
    message(STATUS "deps missing — skipping test_data_device_roundtrip")
endif ()

# data_control_roundtrip — the same helper driven by ext-data-control, proving
# it is protocol-agnostic.  Needs core wayland.xml for wl_seat as well.
if (WAYLAND_SCANNER_DIR AND WAYLAND_PROTOCOLS_DIR
    AND TARGET PkgConfig::wayland_client AND TARGET PkgConfig::wayland_server)
    wlcxx_add_test(data_control_roundtrip 60
        integration/test_data_control_roundtrip.cpp)
    wayland_cxx_generate(
        PROTOCOL ${WAYLAND_SCANNER_DIR}/wayland.xml
        MODE     client-header
        OUTPUT   dc_client.hpp
        EMIT_INTERFACE_TABLES
        TARGET   test_data_control_roundtrip)
    wayland_cxx_generate(
        PROTOCOL ${WAYLAND_PROTOCOLS_DIR}/staging/ext-data-control/ext-data-control-v1.xml
        MODE     client-header
        OUTPUT   dc_ext_client.hpp
        EMIT_INTERFACE_TABLES
        TARGET   test_data_control_roundtrip)
else ()
    message(STATUS "deps missing — skipping test_data_control_roundtrip")
endif ()

# dmabuf_feedback_roundtrip — the feedback helper against a real compositor's
# event sequence.
if (WAYLAND_PROTOCOLS_DIR
    AND TARGET PkgConfig::wayland_client AND TARGET PkgConfig::wayland_server)
    wlcxx_add_test(dmabuf_feedback_roundtrip 60
        integration/test_dmabuf_feedback_roundtrip.cpp)
    wayland_cxx_generate(
        PROTOCOL ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
        MODE     client-header
        OUTPUT   ldbf_client.hpp
        EMIT_INTERFACE_TABLES
        TARGET   test_dmabuf_feedback_roundtrip)
else ()
    message(STATUS "deps missing — skipping test_dmabuf_feedback_roundtrip")
endif ()

# pointer_axis_roundtrip — wl_pointer scroll through the generated thunks, at a
# genuinely negotiated seat version.  seat.hpp pulls in keyboard.hpp, hence
# xkbcommon.
if (WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon
    AND TARGET PkgConfig::wayland_client AND TARGET PkgConfig::wayland_server)
    wlcxx_add_test(pointer_axis_roundtrip 60
        integration/test_pointer_axis_roundtrip.cpp)
    wayland_cxx_generate(
        PROTOCOL ${WAYLAND_SCANNER_DIR}/wayland.xml
        MODE     client-header
        OUTPUT   wayland_client.hpp
        TARGET   test_pointer_axis_roundtrip)
    target_link_libraries(test_pointer_axis_roundtrip PRIVATE PkgConfig::xkbcommon)
else ()
    message(STATUS "deps missing — skipping test_pointer_axis_roundtrip")
endif ()
