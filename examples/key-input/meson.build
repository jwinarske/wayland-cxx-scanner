# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# key-input: keyboard input and repeat demonstration.
#
# Exercises wl::KeyboardHandler keyboard repeat, modeled after
# weston/clients/editor.c: text buffer with cursor navigation,
# printable-character insertion via xkb_keysym_to_utf32, backspace/delete,
# arrow keys (with keyboard repeat), Home, End, and Enter.
#
# Rendering uses wl_shm (XRGB8888) with an inline 8x8 bitmap font; no
# EGL/Mesa/GPU dependency.  Structure mirrors simple-egl exactly.

# ── Dep checks — silently skip if anything is absent ─────────────────────────

wp_dep        = dependency('wayland-protocols', required: false)
wl_scanner_dep = dependency('wayland-scanner',  required: false)
xkbcommon_dep  = dependency('xkbcommon',        required: false)

if not (wp_dep.found() and wl_scanner_dep.found() and xkbcommon_dep.found())
    subdir_done()
endif

# ── Locate system protocol XMLs ───────────────────────────────────────────────

wayland_xml   = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
xdg_shell_xml = wp_dep.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

# ── Generate C++ client headers from protocol XMLs ────────────────────────────

wayland_client_hdr = custom_target(
    'key-input-wayland-client-header',
    input:   wayland_xml,
    output:  'wayland_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_hdr = custom_target(
    'key-input-xdg-shell-client-header',
    input:   xdg_shell_xml,
    output:  'xdg_shell_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include paths ─────────────────────────────────────────────────────────────

framework_inc = include_directories('../../include')
# '.' picks up both the source dir and the build dir, so the generated
# wayland_client.hpp / xdg_shell_client.hpp are found at compile time.
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

executable(
    'key_input',
    sources: [
        'main.cpp',
        wayland_client_hdr,
        xdg_shell_client_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [
        wayland_client_dep,
        xkbcommon_dep,
    ],
    cpp_args: ['-Wno-unused-parameter'],
)
