# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# skia-vulkan-dmabuf: renders the shared demo scene with Skia's Ganesh Vulkan
# backend into VkImages exported as dma-buf (zwp_linux_dmabuf_v1), a zero-copy
# present path.  Descended into only when Skia was resolved (see
# examples/meson.build) and its Vulkan / libdrm deps are present.

vulkan_dep     = dependency('vulkan',             required: false)
libdrm_dep     = dependency('libdrm',             required: false)
wp_dep         = dependency('wayland-protocols',  required: false)
wl_scanner_dep = dependency('wayland-scanner',    required: false)
xkbcommon_dep  = dependency('xkbcommon',          required: false)
# The window frame's cursor loads its theme through wayland-cursor.
wl_cursor_dep  = dependency('wayland-cursor',    required: false)

if not (vulkan_dep.found() and libdrm_dep.found() and wp_dep.found() and
        wl_scanner_dep.found() and xkbcommon_dep.found()
        and wl_cursor_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml           = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml         = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
linux_dmabuf_xml      = wp_dep.get_variable('pkgdatadir') / 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml'
presentation_time_xml = wp_dep.get_variable('pkgdatadir') / 'stable/presentation-time/presentation-time.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'skia-vulkan-dmabuf-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'skia-vulkan-dmabuf-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

linux_dmabuf_client_hdr = custom_target(
    'skia-vulkan-dmabuf-linux-dmabuf-client-header',
    input:   linux_dmabuf_xml,
    output:  'linux_dmabuf_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# presentation-time is not in libwayland, so emit its own interface tables.
presentation_time_client_hdr = custom_target(
    'skia-vulkan-dmabuf-presentation-time-client-header',
    input:   presentation_time_xml,
    output:  'presentation_time_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────
#
# Explicit sync (wp_linux_drm_syncobj) is vk_common_dep's: it generates the
# staging protocol, builds the bridge, and defines HAVE_DRM_SYNCOBJ when the host
# ships it. Absent, vk_common_dep is header-path-only and this falls back to the
# CPU-fence present path.

executable(
    'skia_vulkan_dmabuf',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        linux_dmabuf_client_hdr,
        presentation_time_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
        vulkan_dep,
        libdrm_dep,
        xkbcommon_dep,
        skia_demo_common_dep,
        # The window frame (csd_dep). With csd=none this is only the header path.
        csd_dep,
        wl_cursor_dep,
        # The explicit-sync bridge, and HAVE_DRM_SYNCOBJ when available.
        vk_common_dep,
    ],
    cpp_args: ['-Wno-unused-parameter'],
)
