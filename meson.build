# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

project(
    'wayland-cxx-scanner',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>= 1.1',
    default_options: [
        'cpp_std=c++17',
        'warning_level=3',
        'werror=true',
        'b_ndebug=if-release',
    ],
)

# ── IME backend selection ─────────────────────────────────────────────────────
# Maps the single-valued ime_backend option to a compile define consumed by
# include/wl/ime/backend.hpp.  Exactly one backend is active per build.

ime_backend = get_option('ime_backend')
ime_defines = {
    'none'                : '-DWL_IME_BACKEND_NONE',
    'text-input-v1'       : '-DWL_IME_BACKEND_TEXT_INPUT_V1',
    'text-input-v3'       : '-DWL_IME_BACKEND_TEXT_INPUT_V3',
    'input-method-v1'     : '-DWL_IME_BACKEND_INPUT_METHOD_V1',
    'input-method-v2'     : '-DWL_IME_BACKEND_INPUT_METHOD_V2',
    'virtual-keyboard-v1' : '-DWL_IME_BACKEND_VIRTUAL_KEYBOARD_V1',
}
add_project_arguments(ime_defines[ime_backend], language: 'cpp')

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
    'include/wl/csd_cairo.hpp',
    'include/wl/csd_common.hpp',
    'include/wl/csd_fallback.hpp',
    'include/wl/csd_gtk.hpp',
    'include/wl/csd_plugin.hpp',
    'include/wl/data_device.hpp',
    'include/wl/display.hpp',
    'include/wl/fd_handle.hpp',
    'include/wl/linux_dmabuf.hpp',
    'include/wl/raii.hpp',
    'include/wl/event_map.hpp',
    'include/wl/span.hpp',
    'include/wl/proxy.hpp',
    'include/wl/proxy_impl.hpp',
    'include/wl/wl_ptr.hpp',
    'include/wl/resource_impl.hpp',
    'include/wl/registry.hpp',
    'include/wl/scale_policy.hpp',
    'include/wl/scale_policy_vectors.hpp',
    'include/wl/wayland.hpp',
    'include/wl/xdg_shell.hpp',
    'include/wl/xdg_decoration.hpp',
    'include/wl/keyboard.hpp',
    'include/wl/pointer.hpp',
    'include/wl/present_feedback.hpp',
    'include/wl/presentation.hpp',
    'include/wl/seat.hpp',
    'include/wl/touch.hpp',
    subdir: 'wl',
)

# IME role facade interfaces + backend dispatch header.
install_headers(
    'include/wl/ime/text_input_receiver.hpp',
    'include/wl/ime/input_method.hpp',
    'include/wl/ime/virtual_keyboard.hpp',
    'include/wl/ime/backend.hpp',
    subdir: 'wl/ime',
)

# IME backend implementations (header-only wrappers over generated proxies).
install_headers(
    'include/wl/ime/backends/text_input_v1.hpp',
    'include/wl/ime/backends/text_input_v3.hpp',
    subdir: 'wl/ime/backends',
)

# ── pkg-config for downstream consumers of the framework headers ──────────────

pkg = import('pkgconfig')
pkg.generate(
    name: 'wayland-cxx',
    description: 'WTL-patterned C++17 Wayland framework headers',
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
