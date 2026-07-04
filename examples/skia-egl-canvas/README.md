<!-- SPDX-License-Identifier: MIT -->
# skia-egl-canvas example

Render the shared demo scene with [Skia](https://skia.org/)'s **Ganesh GL**
backend through an EGL / wayland-egl surface. A `GrDirectContext` drives OpenGL
ES; each frame the default framebuffer (FBO 0) is wrapped in an `SkSurface` and
drawn with the same `DemoScene` the [`skia-shm-canvas`](../skia-shm-canvas)
example renders on the CPU, so the two backends are directly comparable.

## Controls

| Key | Action |
|---|---|
| `Esc` / window close | quit |
| `Space` | toggle the button-active scene state |
| left-click | toggle the button when clicked on it (via the view tree's hit test) |
| touch tap | toggle the button when tapped (multi-touch tracked, up to 10 contacts) |

## Build and run

Build Skia and configure the tree as described in
[`../skia-shm-canvas/README.md`](../skia-shm-canvas/README.md); this example
additionally needs EGL, GLESv2, and wayland-egl.

```sh
ninja -C build examples/skia-egl-canvas/skia_egl_canvas
./build/examples/skia-egl-canvas/skia_egl_canvas
```

`skia_egl_canvas` is a Wayland client, so it needs a running compositor.
Headless Weston must expose GL to clients:

```sh
weston --backend=headless --renderer=gl --width=800 --height=600 &
WAYLAND_DISPLAY=wayland-1 ./build/examples/skia-egl-canvas/skia_egl_canvas
```

## Flags

The same pacing flags as [`skia-shm-canvas`](../skia-shm-canvas/README.md#flags):
`--frames N`, `--exit`, `--fixed-dt`, and `--benchmark N`. A bounded run is
self-paced with the swap interval set to 0, so it completes without waiting on
vsync — useful for headless CI and GPU-side perf measurement.

```sh
./build/examples/skia-egl-canvas/skia_egl_canvas --benchmark 300
```
