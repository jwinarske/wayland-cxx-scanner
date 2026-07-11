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

if not (vulkan_dep.found() and libdrm_dep.found() and wp_dep.found() and
        wl_scanner_dep.found() and xkbcommon_dep.found())
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

# ── Optional: explicit sync (wp_linux_drm_syncobj, staging) ────────────────────
# Only when the host's wayland-protocols ships it; otherwise the CPU-fence path
# is the only sync path (HAVE_DRM_SYNCOBJ unset).
_fs = import('fs')
drm_syncobj_xml = wp_dep.get_variable('pkgdatadir') / 'staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml'
extra_sources  = []
extra_cpp_args = ['-Wno-unused-parameter']
if _fs.exists(drm_syncobj_xml)
    drm_syncobj_client_hdr = custom_target(
        'skia-vulkan-dmabuf-drm-syncobj-client-header',
        input:   drm_syncobj_xml,
        output:  'drm_syncobj_client.hpp',
        command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
                  '@INPUT@', '@OUTPUT@'],
    )
    extra_sources  += [drm_syncobj_client_hdr]
    extra_cpp_args += ['-DHAVE_DRM_SYNCOBJ=1']
endif

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'skia_vulkan_dmabuf',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        linux_dmabuf_client_hdr,
        presentation_time_client_hdr,
    ] + extra_sources,
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
        vulkan_dep,
        libdrm_dep,
        xkbcommon_dep,
        skia_demo_common_dep,
    ],
    cpp_args: extra_cpp_args,
)
