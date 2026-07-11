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

**Fallback path.** When no renderable + sampleable modifier is on offer (or the
modifier extensions are absent), Skia draws into its own optimal `SkSurface` and
a `vkCmdCopyImage` blits it into a linear, `DRM_FORMAT_MOD_LINEAR`
dma-buf-exported image — the guaranteed baseline. Set `SKIA_VULKAN_DMABUF_LINEAR`
to force this path where the compositor advertises tiled modifiers.

Both paths are double-buffered and synchronized with a CPU fence before
`wl_surface.commit`. All Vulkan is written with
[Vulkan-Hpp](https://github.com/KhronosGroup/Vulkan-Hpp) (`vk::`), dropping to
raw C handles only at Skia's `VulkanBackendContext` / `GrVkImageInfo` boundary.
Explicit sync (`wp_linux_drm_syncobj_v1`) and buffer-age partial repaint are
follow-ups.

The startup line reports which path engaged, e.g. `… → modifier-tiled dma-buf
direct (modifier 0x0200000000…)` or `… → linear dma-buf present (copy fallback)`.

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
- The producer and the compositor must share a compatible GPU: a
  software-rendered (lavapipe) buffer cannot be imported by a hardware
  compositor, and vice versa.

Build requirements: `vulkan`, `libdrm`, `wayland-protocols`, `wayland-scanner`,
`xkbcommon`, and a Skia build with Vulkan enabled (see
[`scripts/build-skia.sh`](../../scripts/build-skia.sh)). Gated behind the
`skia_examples` meson feature.
