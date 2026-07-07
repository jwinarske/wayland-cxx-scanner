<!-- SPDX-License-Identifier: MIT -->
# skia-vulkan-dmabuf example

Render the shared demo scene with [Skia](https://skia.org/)'s **Ganesh Vulkan**
backend and present it as a **dma-buf** through `zwp_linux_dmabuf_v1`, so the
pixels reach the compositor without ever touching `wl_shm`. It draws the same
`DemoScene` as [`skia-shm-canvas`](../skia-shm-canvas) (CPU raster) and
[`skia-egl-canvas`](../skia-egl-canvas) (Ganesh GL), so the three backends are
directly comparable.

## How it works

Skia's Ganesh backend renders into GPU-optimal images it owns; it does **not**
render into an externally-allocated dma-buf image. So the frame path is:

1. Skia draws the scene into its own optimal `SkSurface`
   (`SkSurfaces::RenderTarget`).
2. A `vkCmdCopyImage` copies that into a linear, dma-buf-exported `VkImage`
   (`VK_KHR_external_memory_fd` + `VK_EXT_external_memory_dma_buf`).
3. The exported fd becomes a `wl_buffer` via `zwp_linux_dmabuf_v1` with
   `DRM_FORMAT_MOD_LINEAR`, which the compositor imports and scans out.

The copy is a GPU pass — there is no CPU readback and no `wl_shm` fallback. All
Vulkan is written with [Vulkan-Hpp](https://github.com/KhronosGroup/Vulkan-Hpp)
(`vk::`), dropping to raw C handles only at Skia's `VulkanBackendContext` /
`GrVkImageInfo` boundary.

This is the **linear baseline**: a single top-level surface, double-buffered,
synchronized with a CPU fence before `wl_surface.commit`. Rendering straight
into a modifier-tiled exported image (single allocation, no copy), an
independent subsurface producer, and explicit sync
(`wp_linux_drm_syncobj_v1`) are follow-ups.

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
  `VK_EXT_external_memory_dma_buf`.
- A Wayland compositor advertising `xdg-shell` and `zwp_linux_dmabuf_v1`.
- The producer and the compositor must share a compatible GPU: a
  software-rendered (lavapipe) buffer cannot be imported by a hardware
  compositor, and vice versa.

Build requirements: `vulkan`, `libdrm`, `wayland-protocols`, `wayland-scanner`,
`xkbcommon`, and a Skia build with Vulkan enabled (see
[`scripts/build-skia.sh`](../../scripts/build-skia.sh)). Gated behind the
`skia_examples` meson feature.
