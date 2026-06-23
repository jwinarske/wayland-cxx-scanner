# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# shadertoy: run any Shadertoy "Image" shader on Wayland.  Two independently
# gated variants share the same agnostic renderer from the shadertoy-cxx
# library:
#   * shadertoy_egl    — EGL / OpenGL ES 3
#   * shadertoy_vulkan — Vulkan (runtime GLSL→SPIR-V)

# ── Common deps ───────────────────────────────────────────────────────────────
wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)
xkbcommon_dep  = dependency('xkbcommon',         required: false)

# The agnostic renderer.  Resolves to a system install (pkg-config) or builds
# from the subprojects/shadertoy-cxx.wrap fallback.
shadertoy_dep = dependency('shadertoy-cxx',
    fallback: ['shadertoy-cxx', 'shadertoy_dep'],
    required: false,
)

if not (wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found() and
        shadertoy_dep.found())
    subdir_done()
endif

# ── Generate C++ client headers from protocol XMLs (shared by both variants) ──
wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

wayland_client_hdr = custom_target(
    'shadertoy-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_client_hdr = custom_target(
    'shadertoy-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── EGL / OpenGL ES 3 variant ─────────────────────────────────────────────────
egl_dep         = dependency('egl',         required: false)
glesv2_dep      = dependency('glesv2',      required: false)
wayland_egl_dep = dependency('wayland-egl', required: false)

if egl_dep.found() and glesv2_dep.found() and wayland_egl_dep.found()
    executable(
        'shadertoy_egl',
        sources: ['main_egl.cpp', wayland_client_hdr, xdg_shell_client_hdr],
        include_directories: [framework_inc, generated_inc],
        dependencies: [
            wayland_client_dep,
            egl_dep,
            glesv2_dep,
            wayland_egl_dep,
            xkbcommon_dep,
            shadertoy_dep,
        ],
        cpp_args: ['-Wno-unused-parameter'],
    )
endif

# ── Vulkan variant ────────────────────────────────────────────────────────────
vulkan_dep = dependency('vulkan', required: false)

if vulkan_dep.found()
    executable(
        'shadertoy_vulkan',
        sources: ['main_vulkan.cpp', wayland_client_hdr, xdg_shell_client_hdr],
        include_directories: [framework_inc, generated_inc],
        dependencies: [
            wayland_client_dep,
            vulkan_dep,
            xkbcommon_dep,
            shadertoy_dep,
        ],
        cpp_args: ['-Wno-unused-parameter'],
    )
endif
