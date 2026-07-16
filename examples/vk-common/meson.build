# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# vk-common: Vulkan helpers shared by the Vulkan examples. Currently the
# wp_linux_drm_syncobj explicit-sync bridge (explicit_sync). Add `vk_common_dep`
# to an example and include <explicit_sync.hpp>; it links only when the host has
# Vulkan, libdrm and the staging drm-syncobj protocol, and defines
# HAVE_DRM_SYNCOBJ so a consumer knows the path is available.
#
# When any of those is absent the dependency is header-path-only and carries no
# define, so a consumer falls back to the CPU-fence present path unchanged.

vk_common_inc = include_directories('.')
framework_inc_vk = include_directories('../../include')

vulkan_dep_vk  = dependency('vulkan',            required: false)
libdrm_dep_vk  = dependency('libdrm',            required: false)
wp_dep_vk      = dependency('wayland-protocols', required: false)
wl_scanner_vk  = dependency('wayland-scanner',   required: false)

_fs = import('fs')
_have_syncobj = false
if wp_dep_vk.found()
    _syncobj_xml = wp_dep_vk.get_variable('pkgdatadir') / 'staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml'
    _have_syncobj = _fs.exists(_syncobj_xml)
endif

if not (vulkan_dep_vk.found() and libdrm_dep_vk.found()
        and wl_scanner_vk.found() and wayland_client_dep.found()
        and _have_syncobj)
    # Nothing to build: no library, no define. The consumer's include of
    # <explicit_sync.hpp> still resolves (header path below), but it never binds
    # the manager, so the caller keeps the CPU-fence path.
    message('vk-common: explicit sync unavailable — CPU-fence path only')
    vk_common_dep = declare_dependency(
        include_directories: vk_common_inc,
    )
    subdir_done()
endif

# The staging protocol, self-contained (--emit-interface-tables) so it defines
# its own wl_interface tables and wl_iface() bodies — the helper needs no
# per-consumer wl_iface for these.
vk_drm_syncobj_hdr = custom_target(
    'vk-common-drm-syncobj-client-header',
    input:   _syncobj_xml,
    output:  'drm_syncobj_client.hpp',
    command: [scanner_exe, '--mode=client-header', '--emit-interface-tables',
              '@INPUT@', '@OUTPUT@'],
)

vk_common_lib = static_library(
    'vk_common',
    sources: ['explicit_sync.cpp', vk_drm_syncobj_hdr],
    include_directories: [vk_common_inc, framework_inc_vk],
    dependencies: [wayland_client_dep, vulkan_dep_vk, libdrm_dep_vk],
    cpp_args: ['-DHAVE_DRM_SYNCOBJ=1'],
)
vk_common_dep = declare_dependency(
    link_with: vk_common_lib,
    include_directories: vk_common_inc,
    dependencies: [vulkan_dep_vk, libdrm_dep_vk],
    compile_args: ['-DHAVE_DRM_SYNCOBJ=1'],
)
