# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# skia-demo-common: the shared scene, scale policy, and damage helpers used by
# the Skia examples so each example stays thin and diffable against its base.
# Gated by the parent examples/meson.build, which only descends here once Skia
# has been resolved into `skia_dep`.

skia_demo_common_inc = include_directories('.')

skia_demo_common_lib = static_library(
    'skia_demo_common',
    sources: [
        'scene.cpp',
        'scale.cpp',
        'damage.cpp',
    ],
    include_directories: skia_demo_common_inc,
    dependencies: [skia_dep],
)

skia_demo_common_dep = declare_dependency(
    link_with: skia_demo_common_lib,
    include_directories: skia_demo_common_inc,
    dependencies: [skia_dep],
)

# Build-health check: render the scene offscreen to a PNG.  No compositor, no
# GPU — if this links and exits 0, the Skia prefix is wired correctly.
skia_demo_scene_check = executable(
    'skia_demo_scene_check',
    sources: ['render_smoke.cpp'],
    dependencies: [skia_demo_common_dep],
)

test(
    'skia-demo-scene-render',
    skia_demo_scene_check,
    args: [meson.current_build_dir() / 'demo-scene.png'],
    suite: 'skia',
)
