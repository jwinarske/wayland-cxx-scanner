<!-- SPDX-License-Identifier: MIT -->
# skia-vulkan-dmabuf example

Render the shared demo scene with [Skia](https://skia.org/)'s **Ganesh Vulkan**
backend and present it as a **dma-buf** through `zwp_linux_dmabuf_v1`, so the
pixels reach the compositor without ever touching `wl_shm`. It draws the same
`DemoScene` as [`skia-shm-canvas`](../skia-shm-canvas) (CPU raster) and
[`skia-egl-canvas`](../skia-egl-canvas) (Ganesh GL), so the three backends are
directly comparable.

## How it works

The example asks the compositor — through `wl::DmabufFeedback`
(`zwp_linux_dmabuf_v1` v4 feedback) — which DRM format modifiers it can scan
out, and picks the presentation path accordingly.

**Direct path (preferred).** It selects a scanout-capable modifier that this GPU
can both render into and sample (`VK_EXT_image_drm_format_modifier`), allocates a
modifier-tiled, dma-buf-exported `VkImage` per slot, wraps it as a Skia
`GrBackendTexture`, and Skia renders the scene **straight into it — no copy**. A
modifier may span several memory planes (e.g. AMD DCC metadata); each plane's
offset and stride is described to `zwp_linux_buffer_params`. Presenting a
scanout modifier lets the compositor promote the surface onto a hardware plane.
If the compositor **re-advertises** feedback (e.g. the surface's plane-assignment
potential changed) with a different best modifier, the slot ring is rebuilt at
the next frame boundary; a re-advertisement with the same modifier is ignored.

**Fallback path.** When no renderable + sampleable modifier is on offer (or the
modifier extensions are absent), Skia draws into its own optimal `SkSurface` and
a `vkCmdCopyImage` blits it into a linear, `DRM_FORMAT_MOD_LINEAR`
dma-buf-exported image — the guaranteed baseline. Set `SKIA_VULKAN_DMABUF_LINEAR`
to force this path where the compositor advertises tiled modifiers.

**Explicit sync.** When the compositor offers `wp_linux_drm_syncobj_v1` and the
device has timeline + external-semaphore-fd, the direct path uses **explicit
GPU sync** instead of a CPU fence: Skia signals a binary semaphore on flush, a
queue submit bridges it to a `drm_syncobj` timeline acquire point the compositor
waits on before sampling, and the compositor signals a release point the client
polls before reusing a slot. So the CPU never blocks on the render. Set
`SKIA_VULKAN_DMABUF_NO_EXPLICIT_SYNC` to fall back to the CPU-fence + implicit
`wl_buffer.release` path. The copy fallback stays CPU-fence synchronized.

**Damage + partial repaint.** The compositor is told only the region that
actually changed (`wl_surface.damage` from the `demo::ViewTree` dirty rects — the
animated spinner every frame, the button and HUD on change). On the direct path
the *render* is clipped to that region too: because a slot was last drawn
`kNumSlots` frames ago, re-rendering it needs only the damage accumulated since —
its own per-slot accumulator plus this frame's — and Ganesh preserves the rest of
the persistent slot (load, not clear). So only ~the spinner's bounding box is
re-rasterized each frame instead of the whole 480×320. The first use of each slot
and every resize repaint in full; `SKIA_VULKAN_DMABUF_NO_PARTIAL` forces full
repaint. The compositor damage stays the *actually changed* rects, which is
smaller than the buffer-age redraw region and correct because those are the only
pixels that differ from the previous frame. The copy fallback renders in full.

All Vulkan is written with
[Vulkan-Hpp](https://github.com/KhronosGroup/Vulkan-Hpp) (`vk::`), dropping to
raw C handles only at Skia's `VulkanBackendContext` / `GrVkImageInfo` boundary
and the `drm_syncobj` bridge.

The startup line reports which path engaged, e.g. `… → modifier-tiled dma-buf
direct (modifier 0x0200000000…, explicit sync)` or `… → linear dma-buf present
(copy fallback, CPU fence)`.

## Controls

| Key | Action |
|---|---|
| `Esc` / window close | quit |
| `Space` / left-click | toggle the button-active scene state |
| `F1` | toggle the performance overlay (also `--hud`) |

## Options

| Flag | Effect |
|---|---|
| `--frames N` | render at most N frames |
| `--exit` | quit once the frame limit is reached |
| `--fixed-dt` | deterministic 60 Hz animation clock |
| `--benchmark N` | render N frames self-paced and print frame-time stats |
| `--hud` | show the performance overlay |
| `--screenshot FILE` | read the first rendered frame back to a PNG (verifies the render path without a compositor grab) |

## Requirements

- A Vulkan device with `VK_KHR_external_memory_fd` and
  `VK_EXT_external_memory_dma_buf`. `VK_EXT_image_drm_format_modifier` (+
  `VK_KHR_image_format_list`) additionally enables the direct path; without them
  the example uses the linear fallback.
- A Wayland compositor advertising `xdg-shell` and `zwp_linux_dmabuf_v1` (v4+ for
  the direct path's modifier feedback; v3 falls back to linear).
  `wp_linux_drm_syncobj_v1` (+ device `VK_KHR_timeline_semaphore`,
  `VK_KHR_external_semaphore_fd`, `VK_EXT_physical_device_drm`) additionally
  enables explicit sync; without them the direct path uses a CPU fence.
- The producer and the compositor must share a compatible GPU: a
  software-rendered (lavapipe) buffer cannot be imported by a hardware
  compositor, and vice versa.

Build requirements: `vulkan`, `libdrm`, `wayland-protocols`, `wayland-scanner`,
`xkbcommon`, and a Skia build with Vulkan enabled (see
[`scripts/build-skia.sh`](../../scripts/build-skia.sh)). Gated behind the
`skia_examples` meson feature.
