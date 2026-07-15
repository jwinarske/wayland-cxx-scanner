# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors

subdir('minimal')
# simple-egl uses subdir_done() internally if EGL/GLES/wayland-egl deps absent.
subdir('simple-egl')
# wayland-info uses subdir_done() internally if wayland-scanner dep is absent.
subdir('wayland-info')
# subsurfaces uses subdir_done() internally if wayland-protocols/scanner absent.
subdir('subsurfaces')
# presentation-shm uses subdir_done() internally if deps absent.
subdir('presentation-shm')
# agl-compositor uses subdir_done() internally if wayland-scanner/xkbcommon absent.
subdir('agl-presentation-shm')
# agl-presentation-egl uses subdir_done() internally if EGL/GLES/wayland-egl/
# wayland-protocols/scanner/xkbcommon deps are absent.
subdir('agl-presentation-egl')
# ivi-shell uses subdir_done() internally if wayland-scanner/xkbcommon absent.
subdir('ivi-presentation-shm')
# simple-shell-egl uses subdir_done() internally if EGL/GLES/wayland-egl/
# wayland-scanner/xkbcommon deps are absent.
subdir('simple-shell-egl')
# key-input uses subdir_done() internally if wayland-protocols/scanner/xkbcommon absent.
subdir('key-input')
# clipboard uses subdir_done() internally if wayland-protocols/scanner/xkbcommon absent.
subdir('clipboard')
# ext-data-control uses subdir_done() internally if wayland-protocols/scanner or
# the staging ext-data-control protocol are absent.
subdir('ext-data-control')
# text-input uses subdir_done() internally unless ime_backend is text-input-v1
# or text-input-v3 and wayland-protocols/scanner are present.
subdir('text-input')
# xdg-csd uses subdir_done() internally if wayland-protocols/scanner/xkbcommon absent.
subdir('xdg-csd')
# xdg-ssd uses subdir_done() internally if wayland-protocols/scanner/xkbcommon absent.
subdir('xdg-ssd')
# xdg-simple-dmabuf-vulkan uses subdir_done() internally if
# vulkan/libdrm/wayland-protocols/xkbcommon deps are absent.
subdir('xdg-simple-dmabuf-vulkan')
# sdl3-presentation-shm uses subdir_done() internally if SDL3/wayland-protocols absent.
subdir('sdl3-presentation-shm')
# shadertoy uses subdir_done() internally if wayland-protocols/scanner/xkbcommon
# or the shadertoy-cxx dependency are absent.
subdir('shadertoy')
# imgui is gated on the 'imgui_examples' feature (disabled by default, because
# enabling it fetches Dear ImGui) and uses subdir_done() internally.
subdir('imgui')

# ── Skia-backed examples (opt-in) ─────────────────────────────────────────────
#
# Gated behind the 'skia_examples' feature so default and examples=true builds
# never require Skia.  Resolution order per the integration plan: pkg-config
# first, then an out-of-tree build prefix via -Dskia_prefix=.  See
# scripts/build-skia.sh, the single source of truth for the pinned build.
#
# The resolved `skia_dep` is consumed by both skia-demo-common and the
# Skia examples that link against it.
have_skia = false
skia_dep  = dependency('', required: false)

skia_feature = get_option('skia_examples')
if not skia_feature.disabled()
    _skia = dependency('skia', required: false)
    if _skia.found()
        skia_dep  = _skia
        have_skia = true
    else
        skia_prefix = get_option('skia_prefix')
        if skia_prefix != ''
            cxx      = meson.get_compiler('cpp')
            skia_lib = skia_prefix / 'lib'
            _core    = cxx.find_library('skia', dirs: [skia_lib],
                                        required: false)
            if _core.found()
                # An official (is_official_build) Skia build keeps the bundled
                # third-party code in separate archives (harfbuzz, icu, png,
                # skcms, wuffs, ...) that libskia references, with circular
                # dependencies among them.  Link every archive in the prefix
                # inside a single --start-group so archive order does not matter,
                # rather than curating a fragile subset.
                _ls = run_command('sh', '-c',
                                  'ls "' + skia_lib + '"/*.a', check: true)
                _archives = _ls.stdout().strip().split('\n')
                _linkargs = ['-Wl,--start-group']
                foreach a : _archives
                    if a != ''
                        _linkargs += a
                    endif
                endforeach
                _linkargs += ['-Wl,--end-group']

                # System libraries the static raster + text path references.
                # zlib/png/etc. are bundled above; freetype and fontconfig are
                # taken from the system (skia_use_system_freetype2 default).
                _sysdeps = [dependency('threads')]
                _dl = cxx.find_library('dl', required: false)
                if _dl.found()
                    _sysdeps += _dl
                endif
                foreach s : ['freetype2', 'fontconfig']
                    _d = dependency(s, required: false)
                    if _d.found()
                        _sysdeps += _d
                    endif
                endforeach

                # Skia headers reference each other as "include/core/...", so the
                # prefix root (not prefix/include) is the search root, and
                # -isystem keeps Skia headers out of -Werror.
                skia_dep = declare_dependency(
                    compile_args: ['-isystem', skia_prefix],
                    link_args: _linkargs,
                    dependencies: _sysdeps,
                )
                have_skia = true
            endif
        endif
    endif

    if not have_skia and skia_feature.enabled()
        error('skia_examples is enabled but Skia was not found. Build it with '
              + 'scripts/build-skia.sh and pass -Dskia_prefix=<prefix>, or '
              + 'install a skia.pc discoverable by pkg-config.')
    endif
endif

if have_skia
    subdir('skia-demo-common')
    subdir('skia-shm-canvas')
    # skia-egl-canvas uses subdir_done() internally if EGL/GLES/wayland-egl deps
    # are absent.
    subdir('skia-egl-canvas')
    # skia-vulkan-dmabuf uses subdir_done() internally if vulkan/libdrm deps are
    # absent.
    subdir('skia-vulkan-dmabuf')
    # skia-skottie-canvas builds only when the Skia prefix has the Skottie module
    # (scripts/build-skia.sh --with-skottie); it self-skips otherwise.
    subdir('skia-skottie-canvas')
endif
