// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// explicit_sync — Vulkan ↔ wp_linux_drm_syncobj explicit sync for a dma-buf
// present path, extracted from the skia-vulkan-dmabuf example so any Vulkan
// client can reuse it.
//
// The CPU-fence present path stalls the rasterizer after the blit
// (vkWaitForFences) so the compositor never samples a half-drawn frame.
// Explicit sync replaces that stall with timeline sync points the compositor
// honors:
//
//   • acquire — the blit submit signals a Vulkan timeline semaphore that is
//     imported from a DRM syncobj; the compositor waits on that point before it
//     samples the buffer.
//   • release — the compositor signals a second DRM syncobj when it is done;
//   the
//     slot is reclaimed by polling that point, which is the explicit-sync
//     analogue of wl_buffer.release — no CPU stall, no cross-thread event.
//
// All the DRM-syncobj + Vulkan-import + protocol plumbing lives behind a PIMPL,
// so this header is free of the generated protocol types and pulls in only the
// Vulkan handles the boundary needs. Raw VkHandles at the boundary keep it a
// light include; the implementation is Vulkan-Hpp.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

struct wl_display;
struct wl_proxy;

namespace wl {
class CRegistry;
}  // namespace wl

namespace wl::vk {

/// The drm_syncobj ↔ Vulkan-timeline bridge for one presenting surface.
///
/// Bring it up with Create(); if it returns null the caller keeps the CPU-fence
/// path. Per present: signal acquire_semaphore() at NextFrame().acquire in the
/// blit submit, then SetSurfacePoints() between wl_surface.attach and .commit;
/// reclaim a slot when SlotReleased() reports its release point signalled.
class ExplicitSync {
 public:
  /// The manager global to watch for during the registry scan; record its name
  /// and hand it to Create(). Kept here so a consumer needs neither the
  /// generated protocol header nor the interface string.
  static constexpr const char* kManagerInterface =
      "wp_linux_drm_syncobj_manager_v1";

  /// A frame's acquire (GPU-signalled) and release (compositor-signalled)
  /// timeline points. Both monotonic; the caller records the release point per
  /// slot and hands it back to SlotReleased().
  struct FramePoints {
    uint64_t acquire = 0;
    uint64_t release = 0;
  };

  /// Bring up the bridge for @p surface.
  ///
  /// Binds the manager from @p registry, creates the two DRM syncobjs, imports
  /// them into the compositor as timelines and the acquire one into Vulkan as a
  /// timeline semaphore, and creates the syncobj surface.
  ///
  /// @param physical_device,device  Already created with timeline-semaphore,
  ///        external-semaphore-fd and physical-device-drm enabled — the device
  ///        extensions cannot be added after the fact, so the caller decides.
  /// @returns null (caller keeps the CPU-fence path) if @p manager_name is 0,
  ///          the render node can't be resolved or opened, or an import fails.
  ///          @p err carries the reason.
  [[nodiscard]] static std::unique_ptr<ExplicitSync> Create(
      VkPhysicalDevice physical_device,
      VkDevice device,
      wl::CRegistry& registry,
      uint32_t manager_name,
      uint32_t manager_version,
      wl_display* display,
      wl_proxy* surface,
      std::string& err);

  ~ExplicitSync();
  ExplicitSync(const ExplicitSync&) = delete;
  ExplicitSync& operator=(const ExplicitSync&) = delete;
  ExplicitSync(ExplicitSync&&) = delete;
  ExplicitSync& operator=(ExplicitSync&&) = delete;

  /// The timeline semaphore the blit submit must signal at
  /// FramePoints::acquire — chain a VkTimelineSemaphoreSubmitInfo whose signal
  /// value is that point, and set this as the submit's signal semaphore.
  [[nodiscard]] VkSemaphore acquire_semaphore() const noexcept;

  /// Allocate this frame's acquire and release points. Call once per present,
  /// before the blit submit.
  [[nodiscard]] FramePoints NextFrame() noexcept;

  /// Attach the frame's acquire and release points to the syncobj surface.
  /// MUST be called between wl_surface.attach and wl_surface.commit.
  void SetSurfacePoints(const FramePoints& points) noexcept;

  /// Non-blocking: has the compositor signalled the release syncobj at
  /// @p release_point yet? The explicit-sync analogue of wl_buffer.release.
  /// A @p release_point of 0 (slot never committed) returns true.
  [[nodiscard]] bool SlotReleased(uint64_t release_point) const noexcept;

 private:
  ExplicitSync();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wl::vk
