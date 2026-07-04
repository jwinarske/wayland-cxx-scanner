<!-- SPDX-License-Identifier: MIT -->
# skia-shm-canvas example

Render a scene with [Skia](https://skia.org/)'s CPU raster backend directly
into a `wl_shm` buffer. Skia wraps the mapped pool memory with a raster surface
(no copy), draws, and the frame is committed with buffer damage under
frame-callback pacing.

The scene, the fractional-scale policy, and the damage helpers live in the
`skia-demo-common` static library so this example stays thin and the same scene
can be shared by future GL and Vulkan variants for direct comparison.

## What it exercises

- **`wl_shm` raster** — `SkSurfaces::WrapPixels` over a 4-deep pool with
  `wl_buffer.release` tracking; the pool is recreated on resize.
- **Fractional scale** — `wp_fractional_scale_v1` + `wp_viewporter` (both
  optional; without them the client stays at integer scale 1). The buffer is
  allocated at physical pixels, the canvas is scaled once, and the viewport
  destination is set to the logical size.
- **Per-view damage** — only the rectangles that changed (the animated spinner
  every frame, the button when it toggles) are mapped to buffer pixels,
  clamped, coalesced, and sent as `wl_surface.damage_buffer`.
- **Text** — an `SkParagraph` card with mixed-weight runs, a color-emoji
  fallback, and an RTL span, shaped via the system fontconfig manager.

## Controls

| Key | Action |
|---|---|
| `Esc` / window close | quit |
| `Space` | toggle the button-active scene state |
| left-click | toggle the button when clicked on it (via the view tree's hit test) |

## Build

The Skia examples are gated behind a disabled-by-default feature; `-Dexamples=true`
alone never requires Skia.

### 1. Build Skia

Skia is a heavyweight out-of-tree dependency and is not vendored.
`scripts/build-skia.sh` is the single source of truth for the pinned revision
(`chrome/m150`) and the canonical GN args. It builds Skia and installs a
self-contained prefix:

```sh
scripts/build-skia.sh --prefix "$HOME/.local/skia-m150"
```

System dev packages are required for the raster + text path (Fedora names):
`freetype-devel fontconfig-devel zlib-devel`, plus `wayland-devel
wayland-protocols-devel libxkbcommon-devel` for the example itself.

The text card's color-emoji and Arabic (RTL) runs need runtime font packages
that ship those glyphs; without them those runs render as blank tofu boxes
while the Latin text is unaffected. Install the color-emoji font (the color 🚀)
and an Arabic font (مرحبا):

- Fedora: `google-noto-emoji-color-fonts google-noto-sans-arabic-fonts`
- Ubuntu/Debian: `fonts-noto-color-emoji fonts-noto-core`
  (`fonts-noto-core` supplies Noto Sans Arabic)

The scene resolves fonts through the system fontconfig manager, so any
equivalent color-emoji and Arabic families work; verify coverage with
`fc-list ':charset=1F680'` and `fc-list ':lang=ar'` (each should list at least
one font).

### 2. Configure and build

```sh
meson setup build -Dexamples=true -Dskia_examples=enabled \
    -Dskia_prefix="$HOME/.local/skia-m150"
ninja -C build examples/skia-shm-canvas/skia_shm_canvas
```

Skia is resolved via pkg-config first, then the `-Dskia_prefix` build prefix.
Enabling `skia_examples` without a discoverable Skia fails with an actionable
error.

## Run

`skia_shm_canvas` is a Wayland client, so it needs a running compositor:

```sh
./build/examples/skia-shm-canvas/skia_shm_canvas
```

Or headless under Weston:

```sh
weston --backend=headless --width=800 --height=600 &
WAYLAND_DISPLAY=wayland-1 ./build/examples/skia-shm-canvas/skia_shm_canvas
```

### Flags

| Flag | Meaning |
|---|---|
| `--frames N` | Render at most N frames. |
| `--exit` | Quit once the frame limit is reached. |
| `--fixed-dt` | Use a deterministic 60 Hz animation clock. |
| `--benchmark N` | Render N frames self-paced (driven by display roundtrips, not frame callbacks) and print frame-time stats (mean, p50/p95/p99). |

A bounded run (`--frames N --exit`, or `--benchmark N`) is self-paced, so it
runs to completion even when the surface is never presented — useful for
headless CI and perf measurement.

## Tests

The `skia-demo-common` library ships offscreen checks and unit tests, all in the
`skia` test suite (`-Dtests=true`):

```sh
meson setup build -Dtests=true -Dexamples=true -Dskia_examples=enabled \
    -Dskia_prefix="$HOME/.local/skia-m150"
meson test -C build --suite skia
```

| Test | What it covers |
|---|---|
| `skia-demo-scale` | fractional-scale buffer sizing and canvas scale |
| `skia-demo-damage` | damage-list clamp and coalesce helpers |
| `skia-demo-frame_pacer` | frame limits, animation clock, frame-time stats |
| `skia-demo-view_tree` | view layout, hit-testing, dirty-region damage |
| `skia-demo-scene-render` | scene renders offscreen to a PNG (links the text stack) |
| `skia-demo-golden` | shapes-only golden-image regression (see below) |

### Golden image

`skia-demo-golden` renders the scene **without the text card's glyphs** — the
system fonts behind the text are not reproducible across machines, but the rest
of the scene is — and compares it against a committed golden within a small
per-channel tolerance:

- Golden: [`../skia-demo-common/golden/scene-shapes.png`](../skia-demo-common/golden/scene-shapes.png)
- Regenerate deliberately (the only way it changes):

  ```sh
  SKIA_REGOLDEN=1 meson test -C build skia-demo-golden
  ```

The golden is tied to the pinned Skia build; bump the golden when bumping the
Skia pin.
