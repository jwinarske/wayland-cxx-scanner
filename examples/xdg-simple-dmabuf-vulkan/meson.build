# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# xdg-simple-dmabuf-vulkan: Vulkan DMA-BUF Wayland client example.
#
# Connects to the running compositor, creates an XDG toplevel window, and
# renders a red/green/blue triangle (matching Weston's simple-dmabuf-vulkan.c)
# into a DMA-BUF-backed Vulkan image exported to the compositor via the
# linux-dmabuf-unstable-v1 protocol.

# ── Dep checks — silently skip if anything is absent ─────────────────────────

vulkan_dep     = dependency('vulkan',            required: false)
libdrm_dep     = dependency('libdrm',            required: false)
wp_dep         = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',   required: false)
xkbcommon_dep  = dependency('xkbcommon',         required: false)

if not (vulkan_dep.found() and libdrm_dep.found() and
        wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml       = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml     = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
linux_dmabuf_xml  = wp_dep.get_variable('pkgdatadir') / \
    'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'dmabuf-vulkan-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'dmabuf-vulkan-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

linux_dmabuf_client_hdr = custom_target(
    'dmabuf-vulkan-linux-dmabuf-client-header',
    input:   linux_dmabuf_xml,
    output:  'linux_dmabuf_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
# '.' picks up both the source dir and the build dir, so the generated
# wayland_client.hpp / xdg_shell_client.hpp / linux_dmabuf_client.hpp are
# found at compile time.
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'xdg_simple_dmabuf_vulkan',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
        linux_dmabuf_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
        vulkan_dep,
        libdrm_dep,
        xkbcommon_dep,
    ],
    # The generated headers use /* param */ comment-suppression for unused
    # virtual-method parameters.  vulkan.hpp also triggers some pedantic
    # warnings on certain compiler versions.
    cpp_args: ['-Wno-unused-parameter'],
)
