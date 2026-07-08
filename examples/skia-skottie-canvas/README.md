<!-- SPDX-License-Identifier: MIT -->
# skia-skottie-canvas example

Play a [Lottie](https://airbnb.io/lottie/) animation with Skia's **Skottie**
module on a bare `wl_shm` surface, with **honest damage tracking** and
**idle-commit suppression**. It shares the [`skia-shm-canvas`](../skia-shm-canvas)
software-raster plumbing (SHM pool, fractional-scale/viewporter policy,
`wp_presentation` timing, seat input) but replaces the demo scene with a
Skottie player.

## The interesting part: honest damage + idle suppression

Each frame the animation is seeked with an `sksg::InvalidationController`, whose
accumulated bounds are the animation's **own** dirty region — straight from the
Lottie scene graph, not a heuristic. That region (mapped to buffer pixels) is
what's handed to `wl_surface.damage_buffer`.

When the animation reaches a **hold** (no keyframes change), the controller
reports an empty region, so the frame is neither rendered nor committed. An
animation-clock `timerfd` keeps polling for the hold to end, but **no commit is
sent** — the commit rate drops to zero, so a held animation costs no rendering,
no buffer, and no protocol traffic until it moves again. The per-second commit
readout printed to the terminal makes this visible:

```
skia-skottie-canvas: 60 commits/s  playhead=1.00s
skia-skottie-canvas: 50 commits/s  playhead=2.00s
skia-skottie-canvas:  0 commits/s  playhead=3.00s  [paused]
```

## No bundled asset

The example ships **no animation** — supply your own Lottie/Bodymovin `.json`
with `--lottie FILE`. Permissively-licensed sample corpora include Skia's own
`resources/skottie/` (BSD) and [`airbnb/lottie-web`](https://github.com/airbnb/lottie-web)
(MIT); verify the license of any individual asset before redistributing it.

## Controls

| Key | Action |
|---|---|
| `Esc` / window close | quit |
| `Space` / left-click / tap | pause / resume |
| `Left` / `Right` | scrub -/+ 1 s |
| `F1` | toggle the performance overlay (also `--hud`) |

> The pointer sets a proper cursor via `wl::CursorManager` (kept crisp at the
> compositor's fractional scale), so binding it no longer leaves a stale
> cursor over the surface.

## Options

| Flag | Effect |
|---|---|
| `--lottie FILE` | Lottie `.json` to play (**required**) |
| `--no-loop` | stop on the last frame instead of looping |
| `--hud` | show the performance overlay |
| `--frames N` | render at most N frames (self-paced) |
| `--exit` | quit once the frame limit is reached |
| `--fixed-dt` | deterministic 60 Hz animation clock |
| `--benchmark N` | render N frames self-paced and print frame-time stats |

## Building

Skottie is **opt-in** in the Skia build because it is not needed by the other
Skia examples. Build the prefix with it enabled:

```sh
scripts/build-skia.sh --prefix <prefix> --with-skottie
meson setup build -Dexamples=true -Dskia_examples=enabled -Dskia_prefix=<prefix>
```

This example self-skips when the resolved Skia prefix has no Skottie module.

> Note: Skottie's public headers pull in Skia's `<bit>`-based math helpers
> (`std::countl_zero`), so this one example compiles at **C++20**; it links
> against the C++17 `skia-demo-common` static library, which is
> ABI-compatible.
