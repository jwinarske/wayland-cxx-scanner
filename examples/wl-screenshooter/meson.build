# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# wl-screenshooter: capture a Wayland compositor output to a PPM file.
#
# Runtime requirement: a Weston compositor with the screenshooter plugin loaded.
#
# Build dependencies:
#   wayland-client  — provided by the parent meson.build
#   wayland-scanner — needed to locate the system wayland.xml, which is used to
#                     generate wayland_client.hpp (CRTP classes for wl_output,
#                     wl_shm, wl_shm_pool, wl_buffer).
#
# If wayland-scanner is absent the sub-directory build is silently skipped via
# subdir_done(), matching the pattern used by the simple-egl example.

# ── Dependency check ──────────────────────────────────────────────────────────

wl_scanner_dep = dependency('wayland-scanner', required: false)
if not wl_scanner_dep.found()
    subdir_done()
endif

# ── Locate system protocol XML ────────────────────────────────────────────────

wayland_xml = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

# wayland_client.hpp — CRTP classes for core Wayland interfaces
# (CWlOutput, CWlShm, CWlShmPool, CWlBuffer, …)
wayland_client_hdr = custom_target(
    'wl-screenshooter-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# screenshooter_client.hpp — CRTP class for weston_screenshooter
screenshooter_client_hdr = custom_target(
    'wl-screenshooter-screenshooter-client-header',
    input:   files('protocol.xml'),
    output:  'screenshooter_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

# Framework headers live under <project_root>/include/wl/.
framework_inc = include_directories('../../include')

# '.' picks up both the source directory and the build directory, so the
# generated wayland_client.hpp / screenshooter_client.hpp are found at compile
# time without an explicit path prefix.
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'wl_screenshooter',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        screenshooter_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep],
    # The generated headers supply default no-op virtual overrides with
    # commented-out parameter names; suppress the resulting warning so
    # -Werror does not break the build.
    cpp_args: ['-Wno-unused-parameter'],
)
