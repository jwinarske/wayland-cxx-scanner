# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Build the minimal client/server roundtrip example.
#
# Steps:
#   1. Use wayland-cxx-scanner (built earlier in src/) to generate the client
#      and server C++ headers from protocol.xml via custom_target().
#   2. Compile server.cpp (server-side CRTP impl) and client.cpp (client-side
#      CRTP impl) as separate translation units — they must not be compiled
#      together because wayland-client.h and wayland-server.h define wl_display
#      differently and cannot safely coexist in one TU.
#   3. Link everything into the `minimal_roundtrip` executable.
#   4. Register it as a meson test so `meson test` exercises it in CI.

# ── Generate protocol headers ──────────────────────────────────────────────────

minimal_client_hdr = custom_target(
    'minimal-client-header',
    input:   files('protocol.xml'),
    output:  'minimal_client.hpp',
    command: [scanner_exe, '--mode=client-header', '@INPUT@', '@OUTPUT@'],
)

minimal_server_hdr = custom_target(
    'minimal-server-header',
    input:   files('protocol.xml'),
    output:  'minimal_server.hpp',
    command: [scanner_exe, '--mode=server-header', '@INPUT@', '@OUTPUT@'],
)

# ── Include-directory helpers ──────────────────────────────────────────────────

# Framework headers live under <project_root>/include/wl/.
framework_inc = include_directories('../../include')

# '.' gives meson both the source dir and the corresponding build dir, so the
# generated minimal_client.hpp / minimal_server.hpp are found at compile time.
generated_inc = include_directories('.')

# ── Executable ────────────────────────────────────────────────────────────────

minimal_roundtrip = executable(
    'minimal_roundtrip',
    sources: [
        'roundtrip.cpp',
        'server.cpp',
        'client.cpp',
        # List the custom_target outputs so meson knows to generate them first.
        minimal_client_hdr,
        minimal_server_hdr,
    ],
    include_directories: [framework_inc, generated_inc],
    dependencies: [wayland_client_dep, wayland_server_dep],
)

# ── Test ──────────────────────────────────────────────────────────────────────

test(
    'minimal_roundtrip',
    minimal_roundtrip,
    timeout: 30,
    suite:   'examples',
)
