# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# CMake port of the examples/*/meson.build files. Each example generates its
# protocol headers with the project's own wayland_cxx_generate() — so this tree
# doubles as the in-repo proof that the public codegen API is ergonomic enough
# for a downstream consumer. Examples are individually dep-gated and silently
# skipped when a dependency is absent, mirroring the Meson subdir_done() guards.

find_package(PkgConfig REQUIRED)

# Protocol XML locations (system data dirs).
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_SCANNER_DIR   wayland-scanner   pkgdatadir)
set(WAYLAND_XML            "${WAYLAND_SCANNER_DIR}/wayland.xml")
set(XDG_SHELL_XML          "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(PRESENTATION_TIME_XML  "${WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml")
set(XDG_OUTPUT_XML         "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml")
set(XDG_DECORATION_XML     "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
set(VIEWPORTER_XML         "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
set(FRACTIONAL_SCALE_XML   "${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml")
set(LINUX_DMABUF_XML       "${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml")
set(EXT_DATA_CONTROL_XML   "${WAYLAND_PROTOCOLS_DIR}/staging/ext-data-control/ext-data-control-v1.xml")
set(AGL_SHELL_XML          "${PROJECT_SOURCE_DIR}/protocols/agl-shell.xml")
set(IVI_APPLICATION_XML    "${PROJECT_SOURCE_DIR}/protocols/ivi-application.xml")
set(SIMPLE_SHELL_XML       "${PROJECT_SOURCE_DIR}/protocols/simpleshell.xml")

# Optional dependency imported targets (PkgConfig::<name> when present).
foreach(_m wayland-client wayland-server wayland-cursor xkbcommon egl glesv2
           wayland-egl vulkan libdrm sdl3 cairo pangocairo shadertoy-cxx)
    string(MAKE_C_IDENTIFIER "${_m}" _id)
    pkg_check_modules(${_id} QUIET IMPORTED_TARGET ${_m})
endforeach()
# gtk+-3.0 mangles to an ugly identifier; give it a clean prefix → PkgConfig::gtk3.
pkg_check_modules(gtk3 QUIET IMPORTED_TARGET gtk+-3.0)

# Create an example exe with the framework + the common warning relaxation.
function(wlcxx_example exe)
    add_executable(${exe} ${ARGN})
    target_link_libraries(${exe} PRIVATE wayland-cxx::wayland-cxx)
    # Generated headers comment-suppress unused virtual-method params, but
    # -Wunused-parameter can still fire under -Werror on some compilers.
    target_compile_options(${exe} PRIVATE -Wno-unused-parameter)
endfunction()

# Generate a client header for <exe> into a per-example subdir (so equally named
# headers across examples never collide). Pass STD / TABLES as trailing flags.
function(wlcxx_gen exe tag outname xml)
    cmake_parse_arguments(_g "TABLES" "STD" "" ${ARGN})
    set(_extra "")
    if (_g_TABLES)
        list(APPEND _extra EMIT_INTERFACE_TABLES)
    endif ()
    if (_g_STD)
        list(APPEND _extra STD ${_g_STD})
    endif ()
    # Generated headers live under a gen/ prefix so the per-example output dir
    # can never collide with an executable's output path of the same name.
    wayland_cxx_generate(
        PROTOCOL "${xml}" MODE client-header
        OUTPUT "gen/${tag}/${outname}" ${_extra} TARGET ${exe})
endfunction()

# ── csd-common — the decoration plugins, shared by any example wanting a frame ─
# Mirrors examples/csd-common/meson.build; keep the two in lock-step.
#
# These are not framework headers: the themed plugins need a .cpp and a toolkit
# on the link line, and the framework is header-only. Only the interface, the
# header-only fallback and the color helpers are installed from include/wl/.
set(CSD_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/csd-common")

# ── CSD resolution — shared by every example that wants a window frame ───────
#
# Resolved once, here, rather than inside one example: the frame is taken by
# more than one of them now, and CMake has no csd_common library for them to
# link the way Meson does — the sources compile into each example, which is also
# what supplies the generated protocol headers they include.
#
# Produces:  _csd_src   sources to add to the example
#            _csd_deps  link libraries
#            _csd_defs  compile definitions
# and wlcxx_csd(<exe>) to apply all three.
# 'none' and 'ssd' outrank the GTK selection: with no plugin compiled there
# is no backend to choose, so asking for both is a contradiction.
if ((WAYLAND_CXX_CSD STREQUAL "none" OR WAYLAND_CXX_CSD STREQUAL "ssd") AND
    (WAYLAND_CXX_CSD_GTK STREQUAL "gtk3" OR WAYLAND_CXX_CSD_GTK STREQUAL "gtk4"))
    message(FATAL_ERROR
        "WAYLAND_CXX_CSD_GTK=${WAYLAND_CXX_CSD_GTK} selects a GTK backend, "
        "but WAYLAND_CXX_CSD=${WAYLAND_CXX_CSD} builds no decoration plugin "
        "for it to back. Set WAYLAND_CXX_CSD=gtk to build the GTK plugin.")
endif ()

# Resolve which GTK the themed plugin builds against.
set(_csd_gtk_found FALSE)
if (WAYLAND_CXX_CSD STREQUAL "none" OR WAYLAND_CXX_CSD STREQUAL "ssd")
    # Skip GTK detection entirely: nothing will link it.
elseif (WAYLAND_CXX_CSD_GTK STREQUAL "gtk4")
    message(FATAL_ERROR
        "WAYLAND_CXX_CSD_GTK=gtk4: the GTK 4 backend is not implemented. "
        "GTK 4 removed the offscreen widget-draw path this plugin renders "
        "through, so it needs a backend of its own. Use gtk3 or auto.")
elseif (WAYLAND_CXX_CSD_GTK STREQUAL "gtk3")
    if (NOT TARGET PkgConfig::gtk3)
        message(FATAL_ERROR "WAYLAND_CXX_CSD_GTK=gtk3: gtk+-3.0 not found.")
    endif ()
    set(_csd_gtk_found TRUE)
elseif (WAYLAND_CXX_CSD_GTK STREQUAL "auto")
    if (TARGET PkgConfig::gtk3)
        set(_csd_gtk_found TRUE)
    endif ()
endif ()

set(_csd_cairo_found FALSE)
if (NOT WAYLAND_CXX_CSD STREQUAL "none" AND NOT WAYLAND_CXX_CSD STREQUAL "ssd"
    AND TARGET PkgConfig::cairo AND TARGET PkgConfig::pangocairo)
    set(_csd_cairo_found TRUE)
endif ()

# Resolve 'auto' to the highest-fidelity plugin available. The runtime
# still prefers the compositor's own decorations and only falls back to the
# plugin if it declines; naming a plugin is what forces client-side.
set(_csd_choice "${WAYLAND_CXX_CSD}")
set(_csd_extra_defs "")
if (_csd_choice STREQUAL "auto")
    set(_csd_extra_defs CSD_PREFER_SSD)
    if (_csd_gtk_found)
        set(_csd_choice gtk)
    elseif (_csd_cairo_found)
        set(_csd_choice cairo)
    else ()
        set(_csd_choice fallback)
    endif ()
endif ()

set(_csd_src   "")
set(_csd_deps  "")
set(_csd_defs  "")
if (_csd_choice STREQUAL "gtk")
    if (NOT _csd_gtk_found)
        if (WAYLAND_CXX_CSD_GTK STREQUAL "disabled")
            message(FATAL_ERROR
                "WAYLAND_CXX_CSD=gtk conflicts with WAYLAND_CXX_CSD_GTK="
                "disabled: the GTK plugin was asked for and gated off at "
                "the same time.")
        endif ()
        message(FATAL_ERROR
            "WAYLAND_CXX_CSD=gtk: gtk+-3.0 not found. Install it, or pick "
            "another plugin with WAYLAND_CXX_CSD=cairo / fallback.")
    endif ()
    set(_csd_src  csd-common/csd_gtk.cpp csd-common/csd_gtk_backend_gtk3.cpp)
    set(_csd_deps PkgConfig::gtk3)
    set(_csd_defs USE_GTK_CSD)
elseif (_csd_choice STREQUAL "cairo")
    if (NOT _csd_cairo_found)
        message(FATAL_ERROR
            "WAYLAND_CXX_CSD=cairo: cairo and pangocairo are both required.")
    endif ()
    set(_csd_src  csd-common/csd_cairo.cpp)
    set(_csd_deps PkgConfig::cairo PkgConfig::pangocairo)
    set(_csd_defs USE_CAIRO_CSD)
elseif (_csd_choice STREQUAL "fallback")
    # Header-only: no source to add, but the define still has to say so, or
    # 'ssd' and 'fallback' would be indistinguishable to the example.
    set(_csd_defs USE_FALLBACK_CSD)
endif ()
# 'none' and 'ssd' add no plugin source and no plugin define.

# csd_factory reads whichever define the choice above set, and
# DecoratedWindow owns the decoration negotiation — which with no plugin is
# the whole of the csd=ssd case. Both are wanted for every choice but
# 'none', so they are appended after the chain rather than inside it. Meson
# builds these into a csd_common library; here they compile into the
# example, which is also what supplies the generated protocol headers they
# include.
#
# 'none' adds neither, and defines nothing: the consumer's
# <decorated_window.hpp> then resolves to empty inlines and no decoration
# code reaches the binary at all.
if (NOT WAYLAND_CXX_CSD STREQUAL "none")
    list(APPEND _csd_src csd-common/csd_factory.cpp csd-common/decorated_window.cpp)
    list(APPEND _csd_defs CSD_ENABLED)
endif ()

list(APPEND _csd_defs ${_csd_extra_defs})
message(STATUS "examples: CSD '${_csd_choice}' "
               "(WAYLAND_CXX_CSD=${WAYLAND_CXX_CSD}, "
               "WAYLAND_CXX_CSD_GTK=${WAYLAND_CXX_CSD_GTK})")

# Give <exe> the decoration the options selected. <tag> is its per-example
# generated-header subdir, as passed to wlcxx_gen().
#
# With csd=none this adds only the include path, and the header found there is
# all empty inlines — no sources, no defines, no protocol, nothing linked.
#
# Otherwise the frame's sources compile into <exe> (Meson has a csd_common
# library; CMake does not), so <exe> is also what must generate the protocol
# headers the frame includes. It already generates wayland_client.hpp and
# xdg_shell_client.hpp for itself; xdg-decoration is the frame's alone, so it is
# generated here rather than left for each example to remember.
function(wlcxx_csd exe tag)
    target_sources(${exe} PRIVATE ${_csd_src})
    target_include_directories(${exe} PRIVATE ${CSD_COMMON_DIR})
    if (_csd_deps)
        target_link_libraries(${exe} PRIVATE ${_csd_deps})
    endif ()
    if (_csd_defs)
        target_compile_definitions(${exe} PRIVATE ${_csd_defs})
    endif ()
    if (NOT WAYLAND_CXX_CSD STREQUAL "none")
        wlcxx_gen(${exe} ${tag} xdg_decoration_unstable_v1_client.hpp
                  ${XDG_DECORATION_XML})
        # The decoration surface's viewport, for fractional scale. Generated
        # here for the same reason as xdg-decoration: it is the frame's, not the
        # example's. An example wanting a viewport of its own must therefore not
        # generate a second copy into the same <tag> dir — the one from here is
        # self-contained (TABLES) and serves both.
        wlcxx_gen(${exe} ${tag} viewporter_client.hpp ${VIEWPORTER_XML} TABLES)
    endif ()
endfunction()

# ── minimal — bundled protocol; client + server roundtrip (also a test) ──────
if (TARGET PkgConfig::wayland_client AND TARGET PkgConfig::wayland_server)
    wlcxx_example(minimal_roundtrip
        minimal/roundtrip.cpp minimal/server.cpp minimal/client.cpp)
    target_link_libraries(minimal_roundtrip PRIVATE
        PkgConfig::wayland_client PkgConfig::wayland_server)
    wayland_cxx_generate(PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/minimal/protocol.xml
        MODE client-header OUTPUT gen/minimal/minimal_client.hpp TARGET minimal_roundtrip)
    wayland_cxx_generate(PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/minimal/protocol.xml
        MODE server-header OUTPUT gen/minimal/minimal_server.hpp TARGET minimal_roundtrip)
    add_test(NAME minimal_roundtrip COMMAND minimal_roundtrip)
    set_tests_properties(minimal_roundtrip PROPERTIES TIMEOUT 30)
endif ()

# ── simple-egl ───────────────────────────────────────────────────────────────
if (TARGET PkgConfig::egl AND TARGET PkgConfig::glesv2 AND TARGET PkgConfig::wayland_egl
    AND WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon
    AND TARGET PkgConfig::wayland_cursor)
    wlcxx_example(simple_egl simple-egl/main.cpp)
    # The window frame. With csd=none this is only an include path and the
    # example carries no decoration code at all.
    wlcxx_csd(simple_egl simple-egl)
    target_link_libraries(simple_egl PRIVATE PkgConfig::wayland_client
        PkgConfig::egl PkgConfig::glesv2 PkgConfig::wayland_egl PkgConfig::xkbcommon
        PkgConfig::wayland_cursor)
    wlcxx_gen(simple_egl simple-egl wayland_client.hpp   ${WAYLAND_XML})
    wlcxx_gen(simple_egl simple-egl xdg_shell_client.hpp ${XDG_SHELL_XML})
endif ()

# ── wayland-info — needs wayland-scanner; wayland-protocols optional ──────────
if (WAYLAND_SCANNER_DIR)
    wlcxx_example(wayland_info wayland-info/wayland-info.cpp)
    target_link_libraries(wayland_info PRIVATE PkgConfig::wayland_client)
    wlcxx_gen(wayland_info wayland-info wayland_client.hpp ${WAYLAND_XML} STD c++17)
    if (WAYLAND_PROTOCOLS_DIR)
        wlcxx_gen(wayland_info wayland-info xdg_output_client.hpp   ${XDG_OUTPUT_XML}   STD c++17)
        wlcxx_gen(wayland_info wayland-info linux_dmabuf_client.hpp ${LINUX_DMABUF_XML} STD c++17)
        target_compile_definitions(wayland_info PRIVATE HAVE_XDG_OUTPUT=1 HAVE_LINUX_DMABUF=1)
    endif ()
endif ()

# ── subsurfaces ──────────────────────────────────────────────────────────────
if (TARGET PkgConfig::egl AND TARGET PkgConfig::glesv2 AND TARGET PkgConfig::wayland_egl
    AND WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(subsurfaces subsurfaces/main.cpp)
    target_link_libraries(subsurfaces PRIVATE PkgConfig::wayland_client
        PkgConfig::egl PkgConfig::glesv2 PkgConfig::wayland_egl PkgConfig::xkbcommon)
    wlcxx_gen(subsurfaces subsurfaces wayland_client.hpp   ${WAYLAND_XML})
    wlcxx_gen(subsurfaces subsurfaces xdg_shell_client.hpp ${XDG_SHELL_XML})
endif ()

# ── presentation-shm ─────────────────────────────────────────────────────────
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(presentation_shm presentation-shm/main.cpp)
    target_link_libraries(presentation_shm PRIVATE PkgConfig::wayland_client PkgConfig::xkbcommon)
    wlcxx_gen(presentation_shm presentation-shm wayland_client.hpp          ${WAYLAND_XML})
    wlcxx_gen(presentation_shm presentation-shm xdg_shell_client.hpp        ${XDG_SHELL_XML})
    wlcxx_gen(presentation_shm presentation-shm presentation_time_client.hpp ${PRESENTATION_TIME_XML} TABLES)
endif ()

# ── agl-presentation-shm ─────────────────────────────────────────────────────
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(agl_compositor agl-presentation-shm/main.cpp)
    target_link_libraries(agl_compositor PRIVATE PkgConfig::wayland_client PkgConfig::xkbcommon)
    wlcxx_gen(agl_compositor agl-presentation-shm wayland_client.hpp   ${WAYLAND_XML})
    wlcxx_gen(agl_compositor agl-presentation-shm xdg_shell_client.hpp ${XDG_SHELL_XML})
    # agl_shell has no pre-built wl_interface symbol, so its header carries the
    # tables (TABLES -> --emit-interface-tables).
    wlcxx_gen(agl_compositor agl-presentation-shm agl_shell_client.hpp ${AGL_SHELL_XML} TABLES)
endif ()

# ── agl-presentation-egl ─────────────────────────────────────────────────────
# The EGL sibling of the above: agl_shell plus presentation-time feedback.
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon
    AND TARGET PkgConfig::egl AND TARGET PkgConfig::glesv2
    AND TARGET PkgConfig::wayland_egl)
    wlcxx_example(agl_presentation_egl agl-presentation-egl/main.cpp)
    target_link_libraries(agl_presentation_egl PRIVATE PkgConfig::wayland_client
        PkgConfig::egl PkgConfig::glesv2 PkgConfig::wayland_egl
        PkgConfig::xkbcommon)
    # The generated headers suppress unused virtual-method parameters with
    # /* param */ comments, which some compiler versions still warn on under
    # the outer -Werror.
    target_compile_options(agl_presentation_egl PRIVATE -Wno-unused-parameter)
    wlcxx_gen(agl_presentation_egl agl-presentation-egl wayland_client.hpp
        ${WAYLAND_XML})
    wlcxx_gen(agl_presentation_egl agl-presentation-egl xdg_shell_client.hpp
        ${XDG_SHELL_XML})
    # Neither agl_shell nor presentation-time has a pre-built wl_interface
    # symbol, so both headers carry their own tables.
    wlcxx_gen(agl_presentation_egl agl-presentation-egl agl_shell_client.hpp
        ${AGL_SHELL_XML} TABLES)
    wlcxx_gen(agl_presentation_egl agl-presentation-egl
        presentation_time_client.hpp ${PRESENTATION_TIME_XML} TABLES)
endif ()

# ── ivi-presentation-shm ─────────────────────────────────────────────────────
if (WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(ivi_shell ivi-presentation-shm/main.cpp)
    target_link_libraries(ivi_shell PRIVATE PkgConfig::wayland_client PkgConfig::xkbcommon)
    wlcxx_gen(ivi_shell ivi-presentation-shm wayland_client.hpp         ${WAYLAND_XML})
    wlcxx_gen(ivi_shell ivi-presentation-shm ivi_application_client.hpp ${IVI_APPLICATION_XML})
endif ()

# ── simple-shell-egl ─────────────────────────────────────────────────────────
if (TARGET PkgConfig::egl AND TARGET PkgConfig::glesv2 AND TARGET PkgConfig::wayland_egl
    AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(simple_shell_egl simple-shell-egl/main.cpp)
    target_link_libraries(simple_shell_egl PRIVATE PkgConfig::wayland_client
        PkgConfig::egl PkgConfig::glesv2 PkgConfig::wayland_egl PkgConfig::xkbcommon)
    wlcxx_gen(simple_shell_egl simple-shell-egl wayland_client.hpp      ${WAYLAND_XML})
    wlcxx_gen(simple_shell_egl simple-shell-egl simple_shell_client.hpp ${SIMPLE_SHELL_XML})
endif ()

# ── key-input ────────────────────────────────────────────────────────────────
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(key_input key-input/main.cpp)
    target_link_libraries(key_input PRIVATE PkgConfig::wayland_client PkgConfig::xkbcommon)
    wlcxx_gen(key_input key-input wayland_client.hpp   ${WAYLAND_XML})
    wlcxx_gen(key_input key-input xdg_shell_client.hpp ${XDG_SHELL_XML})
endif ()

# ── clipboard ────────────────────────────────────────────────────────────────
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(clipboard clipboard/main.cpp)
    target_link_libraries(clipboard PRIVATE PkgConfig::wayland_client PkgConfig::xkbcommon)
    wlcxx_gen(clipboard clipboard wayland_client.hpp   ${WAYLAND_XML})
    wlcxx_gen(clipboard clipboard xdg_shell_client.hpp ${XDG_SHELL_XML})
endif ()

# ── ext-data-control — focus-free clipboard CLI ──────────────────────────────
# No xkbcommon: the protocol is focus-free, so this CLI binds no keyboard.
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND EXISTS "${EXT_DATA_CONTROL_XML}"
    AND TARGET PkgConfig::wayland_client)
    wlcxx_example(ext-data-control ext-data-control/main.cpp)
    target_link_libraries(ext-data-control PRIVATE PkgConfig::wayland_client)
    wlcxx_gen(ext-data-control ext-data-control wayland_client.hpp         ${WAYLAND_XML})
    wlcxx_gen(ext-data-control ext-data-control ext_data_control_client.hpp ${EXT_DATA_CONTROL_XML} TABLES)
endif ()

# ── text-input — text-input-v1/v3 backends only ──────────────────────────────
if (WAYLAND_CXX_IME_BACKEND MATCHES "^text-input-v[13]$"
    AND WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::wayland_client)
    if (WAYLAND_CXX_IME_BACKEND STREQUAL "text-input-v3")
        set(_ti_xml ${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v3.xml)
        set(_ti_out text_input_v3_client.hpp)
    else ()
        set(_ti_xml ${WAYLAND_PROTOCOLS_DIR}/unstable/text-input/text-input-unstable-v1.xml)
        set(_ti_out text_input_v1_client.hpp)
    endif ()
    wlcxx_example(text_input text-input/main.cpp)
    target_link_libraries(text_input PRIVATE PkgConfig::wayland_client)
    wlcxx_gen(text_input text-input wayland_client.hpp   ${WAYLAND_XML})
    wlcxx_gen(text_input text-input xdg_shell_client.hpp ${XDG_SHELL_XML})
    wlcxx_gen(text_input text-input ${_ti_out} ${_ti_xml} TABLES)
endif ()

# ── xdg-csd — the CSD example ────────────────────────────────────────────────
# Not built at csd=none: this example exists to demonstrate client-side
# decoration and negotiates the mode itself, so a build that asked for no
# decoration code has no use for it. Skipped rather than built inert.
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon
    AND TARGET PkgConfig::wayland_cursor AND NOT WAYLAND_CXX_CSD STREQUAL "none")
    wlcxx_example(xdg_csd xdg-csd/main.cpp)
    wlcxx_csd(xdg_csd xdg-csd)
    target_link_libraries(xdg_csd PRIVATE PkgConfig::wayland_client
        PkgConfig::wayland_cursor PkgConfig::xkbcommon)
    wlcxx_gen(xdg_csd xdg-csd wayland_client.hpp                  ${WAYLAND_XML})
    wlcxx_gen(xdg_csd xdg-csd xdg_shell_client.hpp               ${XDG_SHELL_XML})
    # Extension protocols: TABLES keeps their headers self-contained.
    # viewporter comes from wlcxx_csd() above — this example uses it for its own
    # content surface, but the frame needs it too, and generating a second copy
    # into the same dir would be two rules for one output.
    wlcxx_gen(xdg_csd xdg-csd fractional_scale_client.hpp ${FRACTIONAL_SCALE_XML} TABLES)
endif ()

# ── xdg-ssd — server-side decorations; compositor draws the frame (Weston) ────
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon)
    wlcxx_example(xdg_ssd xdg-ssd/main.cpp)
    target_link_libraries(xdg_ssd PRIVATE PkgConfig::wayland_client PkgConfig::xkbcommon)
    wlcxx_gen(xdg_ssd xdg-ssd wayland_client.hpp                     ${WAYLAND_XML})
    wlcxx_gen(xdg_ssd xdg-ssd xdg_shell_client.hpp                  ${XDG_SHELL_XML})
    wlcxx_gen(xdg_ssd xdg-ssd xdg_decoration_unstable_v1_client.hpp ${XDG_DECORATION_XML})
endif ()

# ── xdg-simple-dmabuf-vulkan ─────────────────────────────────────────────────
if (TARGET PkgConfig::vulkan AND TARGET PkgConfig::libdrm
    AND WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon
    AND TARGET PkgConfig::wayland_cursor)
    wlcxx_example(xdg_simple_dmabuf_vulkan xdg-simple-dmabuf-vulkan/main.cpp)
    # The window frame. With csd=none this is only an include path and the
    # example carries no decoration code at all; the cursor still loads.
    wlcxx_csd(xdg_simple_dmabuf_vulkan xdg-simple-dmabuf-vulkan)
    target_link_libraries(xdg_simple_dmabuf_vulkan PRIVATE PkgConfig::wayland_client
        PkgConfig::vulkan PkgConfig::libdrm PkgConfig::xkbcommon
        PkgConfig::wayland_cursor)
    wlcxx_gen(xdg_simple_dmabuf_vulkan xdg-simple-dmabuf-vulkan wayland_client.hpp    ${WAYLAND_XML})
    wlcxx_gen(xdg_simple_dmabuf_vulkan xdg-simple-dmabuf-vulkan xdg_shell_client.hpp  ${XDG_SHELL_XML})
    wlcxx_gen(xdg_simple_dmabuf_vulkan xdg-simple-dmabuf-vulkan linux_dmabuf_client.hpp ${LINUX_DMABUF_XML})
endif ()

# ── sdl3-presentation-shm ────────────────────────────────────────────────────
if (TARGET PkgConfig::sdl3 AND WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR)
    wlcxx_example(sdl3_presentation_shm sdl3-presentation-shm/main.cpp)
    target_link_libraries(sdl3_presentation_shm PRIVATE PkgConfig::wayland_client PkgConfig::sdl3)
    wlcxx_gen(sdl3_presentation_shm sdl3-presentation-shm presentation_time_client.hpp ${PRESENTATION_TIME_XML} TABLES)
endif ()

# ── shadertoy — EGL and/or Vulkan variants, both need shadertoy-cxx ──────────
if (WAYLAND_PROTOCOLS_DIR AND WAYLAND_SCANNER_DIR AND TARGET PkgConfig::xkbcommon
    AND TARGET PkgConfig::shadertoy_cxx)
    if (TARGET PkgConfig::egl AND TARGET PkgConfig::glesv2 AND TARGET PkgConfig::wayland_egl)
        wlcxx_example(shadertoy_egl shadertoy/main_egl.cpp)
        target_link_libraries(shadertoy_egl PRIVATE PkgConfig::wayland_client
            PkgConfig::wayland_cursor PkgConfig::egl PkgConfig::glesv2
            PkgConfig::wayland_egl PkgConfig::xkbcommon PkgConfig::shadertoy_cxx)
        wlcxx_gen(shadertoy_egl shadertoy-egl wayland_client.hpp   ${WAYLAND_XML})
        wlcxx_gen(shadertoy_egl shadertoy-egl xdg_shell_client.hpp ${XDG_SHELL_XML})
    endif ()
    if (TARGET PkgConfig::vulkan)
        wlcxx_example(shadertoy_vulkan shadertoy/main_vulkan.cpp)
        target_link_libraries(shadertoy_vulkan PRIVATE PkgConfig::wayland_client
            PkgConfig::wayland_cursor PkgConfig::vulkan PkgConfig::xkbcommon
            PkgConfig::shadertoy_cxx)
        wlcxx_gen(shadertoy_vulkan shadertoy-vulkan wayland_client.hpp   ${WAYLAND_XML})
        wlcxx_gen(shadertoy_vulkan shadertoy-vulkan xdg_shell_client.hpp ${XDG_SHELL_XML})
    endif ()
endif ()
