# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# wayland-info: print information about the Wayland compositor's globals.
#
# Connects to the running compositor, enumerates all globals advertised via
# wl_registry, and for well-known interfaces (wl_shm, wl_seat, wl_output,
# zxdg_output_manager_v1, zwp_linux_dmabuf_v1) binds and prints details.
#
# Reference:
#   https://gitlab.freedesktop.org/wayland/wayland-utils/-/blob/main/wayland-info/wayland-info.c

# ── wayland-scanner — provides wayland.xml path ───────────────────────────────

wl_scanner_dep = dependency('wayland-scanner', required: false)

if not wl_scanner_dep.found()
    subdir_done()
endif

wayland_xml = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'

# ── wayland-protocols — optional; adds xdg-output and linux-dmabuf support ───

wp_dep = dependency('wayland-protocols', required: false)

# ── Generate C++23 client header from wayland.xml ─────────────────────────────
#
# --std=c++23 enables explicit-object parameters (P0847R7 deducing-this) and
# std::is_class_v requires-constraints in the generated code, matching the
# project-wide -std=c++23 default_option.

wayland_client_hdr = custom_target(
    'wayland-info-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [
        scanner_exe,
        '--mode=client-header',
        '--std=c++23',
        '@INPUT@',
        '@OUTPUT@',
    ],
)

# ── Extended protocol headers (wayland-protocols) ─────────────────────────────

extra_sources  = []
extra_cpp_args = ['-Wno-unused-parameter']

if wp_dep.found()
    wp_datadir = wp_dep.get_variable('pkgdatadir')

    # zxdg_output_manager_v1 / zxdg_output_v1 — logical display position & size
    xdg_output_hdr = custom_target(
        'wayland-info-xdg-output-client-header',
        input:   wp_datadir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
        output:  'xdg_output_client.hpp',
        command: [
            scanner_exe,
            '--mode=client-header',
            '--std=c++23',
            '@INPUT@',
            '@OUTPUT@',
        ],
    )

    # zwp_linux_dmabuf_v1 — DRM pixel-format and modifier advertising
    linux_dmabuf_hdr = custom_target(
        'wayland-info-linux-dmabuf-client-header',
        input:   wp_datadir / 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml',
        output:  'linux_dmabuf_client.hpp',
        command: [
            scanner_exe,
            '--mode=client-header',
            '--std=c++23',
            '@INPUT@',
            '@OUTPUT@',
        ],
    )

    extra_sources  += [xdg_output_hdr, linux_dmabuf_hdr]
    extra_cpp_args += ['-DHAVE_XDG_OUTPUT=1', '-DHAVE_LINUX_DMABUF=1']
endif

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
# '.' gives meson both the source dir and the build dir, so all generated
# headers (wayland_client.hpp, xdg_output_client.hpp, …) are found at compile
# time without explicit per-file include_directories() calls.
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'wayland_info',
    sources: ['wayland-info.cpp', wayland_client_hdr] + extra_sources,
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep],
    # The generated C++23 headers use /* param */ comment-suppression for the
    # default-empty virtual-method overrides, but -Wunused-parameter (part of
    # -Wall / -Wextra) can still trigger on some compiler versions under
    # -Werror.  Suppress only that warning to keep the rest of -Werror clean.
    cpp_args: extra_cpp_args,
)
