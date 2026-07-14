# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Build definition for the Dear ImGui subproject (upstream ships none).
# Applied by subprojects/imgui.wrap via patch_directory.
#
# Deliberately minimal: core plus the one renderer backend examples/imgui uses.
# No platform backend is built — examples/imgui *is* the platform backend, and
# pulling in GLFW or SDL would defeat the point of it.

project(
    'imgui',
    'cpp',
    version: '1.92.8',
    license: 'MIT',
    default_options: ['cpp_std=c++17'],
    meson_version: '>=1.1',
)

imgui_inc = include_directories('.', 'backends', 'misc/cpp')

imgui_sources = files(
    'imgui.cpp',
    'imgui_demo.cpp',
    'imgui_draw.cpp',
    'imgui_tables.cpp',
    'imgui_widgets.cpp',
    'misc/cpp/imgui_stdlib.cpp',
)

# The renderer backend, built for OpenGL ES 2 to match the EGL context the
# example creates.  Upstream picks its GL loader at compile time, so the define
# has to be here rather than on the consumer.
gles_dep = dependency('glesv2', required: true)

imgui_lib = static_library(
    'imgui',
    sources: imgui_sources + files('backends/imgui_impl_opengl3.cpp'),
    include_directories: imgui_inc,
    dependencies: [gles_dep],
    cpp_args: ['-DIMGUI_IMPL_OPENGL_ES2'],
    # Upstream's own warnings are not this project's to fix.
    override_options: ['warning_level=1', 'werror=false'],
)

imgui_dep = declare_dependency(
    link_with: imgui_lib,
    include_directories: imgui_inc,
    dependencies: [gles_dep],
    compile_args: ['-DIMGUI_IMPL_OPENGL_ES2'],
)

meson.override_dependency('imgui', imgui_dep)
