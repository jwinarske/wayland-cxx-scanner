# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

project(
    'wayland-cxx-scanner',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>= 1.1',
    default_options: [
        'cpp_std=c++23',
        'warning_level=3',
        'werror=true',
        'b_ndebug=if-release',
    ],
)

# ── Dependencies ──────────────────────────────────────────────────────────────

wayland_client_dep = dependency('wayland-client', required: false)
wayland_server_dep = dependency('wayland-server', required: false)
pugixml_dep        = dependency('pugixml',
    required: true,
    fallback: ['pugixml', 'pugixml_dep'],
    default_options: ['default_library=static']
)

# ── Framework headers — install to ${includedir}/wl/ ─────────────────────────

install_headers(
    'include/wl/client_helpers.hpp',
    'include/wl/csd_fallback.hpp',
    'include/wl/csd_gtk.hpp',
    'include/wl/csd_plugin.hpp',
    'include/wl/display.hpp',
    'include/wl/fd_handle.hpp',
    'include/wl/linux_dmabuf.hpp',
    'include/wl/raii.hpp',
    'include/wl/event_map.hpp',
    'include/wl/proxy.hpp',
    'include/wl/proxy_impl.hpp',
    'include/wl/wl_ptr.hpp',
    'include/wl/resource_impl.hpp',
    'include/wl/registry.hpp',
    'include/wl/wayland.hpp',
    'include/wl/xdg_shell.hpp',
    'include/wl/xdg_decoration.hpp',
    'include/wl/keyboard.hpp',
    'include/wl/seat.hpp',
    subdir: 'wl',
)

# ── pkg-config for downstream consumers of the framework headers ──────────────

pkg = import('pkgconfig')
pkg.generate(
    name: 'wayland-cxx',
    description: 'WTL-patterned C++23 Wayland framework headers',
    version: meson.project_version(),
    requires: [],      # header-only; consumers link wayland-client/server themselves
)

# ── Scanner tool and library ──────────────────────────────────────────────────

subdir('src')

# ── Tests ─────────────────────────────────────────────────────────────────────

if get_option('tests')
    subdir('tests')
endif

# ── Examples ─────────────────────────────────────────────────────────────────

if get_option('examples') and wayland_client_dep.found() and wayland_server_dep.found()
    subdir('examples')
endif

# ── Doxygen HTML documentation ────────────────────────────────────────────────

if get_option('docs')
    doxygen = find_program('doxygen', required: true)
    dot     = find_program('dot',     required: false)

    doxyfile = configure_file(
        input:         'Doxyfile.in',
        output:        'Doxyfile',
        configuration: {
            'VERSION':     meson.project_version(),
            'SOURCE_ROOT': meson.project_source_root(),
            'OUTPUT_DIR':  meson.project_build_root() / 'docs',
            'HAVE_DOT':    dot.found() ? 'YES' : 'NO',
        },
    )

    custom_target(
        'docs',
        input:   doxyfile,
        output:  'html',
        command: [doxygen, doxyfile],
        build_by_default: true,
        install:          false,
    )
endif
