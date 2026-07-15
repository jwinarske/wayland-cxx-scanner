# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# imgui — Dear ImGui platform backend for Wayland, built on the generated CRTP
# bindings and the wl/ framework, plus a demo application driving it.
#
# Gated behind -Dimgui_examples=enabled: enabling it fetches Dear ImGui through
# subprojects/imgui.wrap (or uses a system one), and examples=true alone must
# never pull an out-of-tree dependency.
#
# The platform backend here is the counterpart to imgui_impl_glfw / _sdl3: it
# owns input (seat, keyboard, pointer, touch, cursor) while the application
# keeps the display, surface, xdg-shell and event loop.  Rendering is upstream's
# imgui_impl_opengl3 over EGL / OpenGL ES 2.

imgui_opt = get_option('imgui_examples')

egl_dep         = dependency('egl',         required: false)
glesv2_dep      = dependency('glesv2',      required: false)
wayland_egl_dep = dependency('wayland-egl', required: false)
xkbcommon_dep   = dependency('xkbcommon',   required: false)
wl_cursor_dep   = dependency('wayland-cursor', required: false)
wp_dep          = dependency('wayland-protocols', required: false)
wl_scanner_dep  = dependency('wayland-scanner',  required: false)

_have_deps = (egl_dep.found() and glesv2_dep.found() and wayland_egl_dep.found()
    and xkbcommon_dep.found() and wl_cursor_dep.found() and wp_dep.found()
    and wl_scanner_dep.found())

if imgui_opt.disabled() or not _have_deps
    if imgui_opt.enabled() and not _have_deps
        error('imgui_examples=enabled but egl/glesv2/wayland-egl/xkbcommon/wayland-cursor/wayland-protocols/wayland-scanner are missing')
    endif
    subdir_done()
endif

# ── Dear ImGui ────────────────────────────────────────────────────────────────
# subprojects/imgui.wrap fetches the release and applies our own build
# definition (subprojects/packagefiles/imgui), which builds core plus
# imgui_impl_opengl3 for GLES 2 and no platform backend — this example is the
# platform backend.  A system imgui, if one is ever packaged, wins.
imgui_dep = dependency('imgui',
    fallback: ['imgui', 'imgui_dep'],
    required: imgui_opt,
)

if not imgui_dep.found()
    subdir_done()
endif

# ── Generated protocol headers ────────────────────────────────────────────────

wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
viewporter_xml       = wp_dep.get_variable('pkgdatadir') / 'stable/viewporter/viewporter.xml'
fractional_scale_xml = wp_dep.get_variable('pkgdatadir') / 'staging/fractional-scale/fractional-scale-v1.xml'

imgui_wayland_client_hdr = custom_target(
    'imgui-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

imgui_xdg_shell_client_hdr = custom_target(
    'imgui-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# viewporter and fractional-scale are extension protocols, so their headers are
# generated with --emit-interface-tables to stay self-contained (they define
# their own wl_interface tables and wl_iface()).
imgui_viewporter_client_hdr = custom_target(
    'imgui-viewporter-client-header',
    input:   viewporter_xml,
    output:  'viewporter_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

imgui_fractional_scale_client_hdr = custom_target(
    'imgui-fractional-scale-client-header',
    input:   fractional_scale_xml,
    output:  'fractional_scale_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

# The IME facade resolves to the build-selected text-input backend, and that
# backend's header includes the matching generated protocol header — so
# generate whichever one the option chose.  The backend compiles its IME path
# out entirely for the other choices (including 'none'), falling back to plain
# keysym text input.
ime_backend = get_option('ime_backend')
imgui_ime_hdr = []
if ime_backend == 'text-input-v3'
    imgui_ime_hdr = custom_target(
        'imgui-text-input-v3-client-header',
        input:   wp_dep.get_variable('pkgdatadir') / 'unstable/text-input/text-input-unstable-v3.xml',
        output:  'text_input_v3_client.hpp',
        command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
                  '@INPUT@', '@OUTPUT@'],
    )
elif ime_backend == 'text-input-v1'
    imgui_ime_hdr = custom_target(
        'imgui-text-input-v1-client-header',
        input:   wp_dep.get_variable('pkgdatadir') / 'unstable/text-input/text-input-unstable-v1.xml',
        output:  'text_input_v1_client.hpp',
        command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
                  '@INPUT@', '@OUTPUT@'],
    )
endif

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Platform backend ──────────────────────────────────────────────────────────

imgui_backend_lib = static_library(
    'imgui_impl_wayland_cxx',
    sources: ['imgui_impl_wayland_cxx.cpp', imgui_wayland_client_hdr]
        + imgui_ime_hdr,
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        imgui_dep,
        wayland_client_dep,
        wl_cursor_dep,
        xkbcommon_dep,
    ],
    # The generated headers name unused virtual-method parameters in comments,
    # which some compiler versions still warn on under the outer -Werror.
    cpp_args: ['-Wno-unused-parameter'],
)

imgui_backend_dep = declare_dependency(
    link_with: imgui_backend_lib,
    include_directories: [include_directories('.'), generated_inc],
)

# ── Demo application ──────────────────────────────────────────────────────────

executable(
    'imgui_demo_wayland',
    sources: [
        'main.cpp',
        imgui_wayland_client_hdr,
        imgui_xdg_shell_client_hdr,
        imgui_viewporter_client_hdr,
        imgui_fractional_scale_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        imgui_backend_dep,
        imgui_dep,
        wayland_client_dep,
        wl_cursor_dep,
        xkbcommon_dep,
        egl_dep,
        glesv2_dep,
        wayland_egl_dep,
    ],
    cpp_args: ['-Wno-unused-parameter'],
)
