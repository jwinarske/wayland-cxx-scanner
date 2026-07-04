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

# ── Golden-image regression ───────────────────────────────────────────────────
#
# Renders the scene without its (system-font) text card and compares it against
# a committed golden PNG.  Regenerate the golden deliberately with:
#   SKIA_REGOLDEN=1 meson test -C build skia-demo-golden
skia_demo_golden_check = executable(
    'skia_demo_golden_check',
    sources: ['golden_check.cpp'],
    dependencies: [skia_demo_common_dep],
)

test(
    'skia-demo-golden',
    skia_demo_golden_check,
    args: [meson.current_source_dir() / 'golden' / 'scene-shapes.png'],
    suite: 'skia',
)

# ── Unit tests ────────────────────────────────────────────────────────────────
#
# Cover the pure logic in the shared library — the fractional-scale policy and
# the damage-list helpers.  Built with -Dtests=true; they live here rather than
# under tests/ because that subdir is processed before Skia is resolved.
if get_option('tests')
    gtest_main_dep = dependency('gtest_main', required: false,
                                fallback: ['gtest', 'gtest_main_dep'])
    if gtest_main_dep.found()
        foreach case : ['scale', 'damage']
            _exe = executable(
                'skia_demo_test_' + case,
                sources: ['test_' + case + '.cpp'],
                dependencies: [skia_demo_common_dep, gtest_main_dep],
            )
            test(
                'skia-demo-' + case,
                _exe,
                args: ['--gtest_color=yes'],
                suite: 'skia',
            )
        endforeach
    else
        warning('GoogleTest not available — skipping Skia demo-common unit tests')
    endif
endif
