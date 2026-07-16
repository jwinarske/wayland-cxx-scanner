// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// explicit_sync — see explicit_sync.hpp.

#include "explicit_sync.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

// Vulkan-Hpp for everything but the boundary handles the header exposes.
// No-exceptions mode: the create/import calls return a ResultValue this checks,
// matching the Vulkan examples that consume this.
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>

// POSIX/DRM headers last: <filesystem> and Vulkan-Hpp pull in glibc headers
// that #undef the major()/minor() macros, so <sys/sysmacros.h> must come after
// them for those macros to survive to use.
extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
}

// The generated protocol header, then the framework helpers it needs. Order
// matters: the traits come first, then wl::construct / CRegistry / WlPtr use
// them. --emit-interface-tables makes it self-contained (its own wl_iface).
#include "drm_syncobj_client.hpp"

#include <wl/proxy_impl.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-core.h>
}

namespace wl::vk {
namespace {

using namespace linux_drm_syncobj_v1::client;

// The wp_linux_drm_syncobj objects carry no events — bare handlers.
class ManagerHandler : public CWpLinuxDrmSyncobjManagerV1<ManagerHandler> {};
class SurfaceHandler : public CWpLinuxDrmSyncobjSurfaceV1<SurfaceHandler> {};
class TimelineHandler : public CWpLinuxDrmSyncobjTimelineV1<TimelineHandler> {};

// Resolve a DRM major:minor to its /dev/dri node path (empty if none).
std::string DrmNodePath(unsigned major_n, unsigned minor_n) {
  std::error_code ec;
  for (const std::filesystem::directory_entry& e :
       std::filesystem::directory_iterator("/dev/dri", ec)) {
    struct stat st{};
    if (::stat(e.path().c_str(), &st) == 0 && S_ISCHR(st.st_mode) &&
        major(st.st_rdev) == major_n && minor(st.st_rdev) == minor_n)
      return e.path().string();
  }
  return {};
}

}  // namespace

// ── Impl ─────────────────────────────────────────────────────────────────────

struct ExplicitSync::Impl {
  ::vk::Device device;
  ::vk::Semaphore acquire_sem;  // timeline; imported from acquire_syncobj
  wl_display* display = nullptr;

  wl::WlPtr<ManagerHandler> manager;
  wl::WlPtr<TimelineHandler> acquire_tl;  // wp timeline (acquire)
  wl::WlPtr<TimelineHandler> release_tl;  // wp timeline (release)
  wl::WlPtr<SurfaceHandler> syncobj_surface;

  int drm_fd = -1;               // render node, for drmSyncobj* ioctls
  uint32_t acquire_syncobj = 0;  // drm handle (Vulkan signals it)
  uint32_t release_syncobj = 0;  // drm handle (compositor signals it)
  uint64_t acquire_point = 0;    // monotonic acquire timeline value
  uint64_t release_point = 0;    // monotonic release timeline value

  ~Impl() {
    if (device)
      (void)device.waitIdle();
    if (acquire_sem)
      device.destroySemaphore(acquire_sem);
    syncobj_surface.Reset();
    acquire_tl.Reset();
    release_tl.Reset();
    manager.Reset();
    if (drm_fd >= 0) {
      if (acquire_syncobj != 0)
        drmSyncobjDestroy(drm_fd, acquire_syncobj);
      if (release_syncobj != 0)
        drmSyncobjDestroy(drm_fd, release_syncobj);
      ::close(drm_fd);
    }
  }
};

ExplicitSync::ExplicitSync() : impl_(std::make_unique<Impl>()) {}
ExplicitSync::~ExplicitSync() = default;

// ── Create ───────────────────────────────────────────────────────────────────

std::unique_ptr<ExplicitSync> ExplicitSync::Create(VkPhysicalDevice physical,
                                                   VkDevice device,
                                                   wl::CRegistry& registry,
                                                   uint32_t manager_name,
                                                   uint32_t manager_version,
                                                   wl_display* display,
                                                   wl_proxy* surface,
                                                   std::string& err) {
  using mgr = wp_linux_drm_syncobj_manager_v1_traits;

  if (manager_name == 0) {
    err = "no wp_linux_drm_syncobj_manager_v1";
    return nullptr;
  }
  if (physical == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
      surface == nullptr) {
    err = "missing Vulkan device or surface";
    return nullptr;
  }

  auto es = std::unique_ptr<ExplicitSync>(new ExplicitSync());
  Impl& s = *es->impl_;
  s.device = ::vk::Device(device);
  s.display = display;

  // Bind the manager from the caller's registry.
  if (wl_proxy* raw = registry.Bind<mgr>(
          manager_name, std::min(manager_version, mgr::version))) {
    s.manager.Attach(raw);
  } else {
    err = "wp_linux_drm_syncobj_manager_v1 bind failed";
    return nullptr;
  }

  // Open the Vulkan device's DRM render node for the syncobj ioctls.
  const ::vk::PhysicalDevice phys(physical);
  ::vk::PhysicalDeviceDrmPropertiesEXT drm{};
  ::vk::PhysicalDeviceProperties2 p2{};
  p2.pNext = &drm;
  phys.getProperties2(&p2);
  const std::string node =
      drm.hasRender ? DrmNodePath(static_cast<unsigned>(drm.renderMajor),
                                  static_cast<unsigned>(drm.renderMinor))
                    : std::string{};
  if (node.empty()) {
    err = "no DRM render node";
    return nullptr;
  }
  s.drm_fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
  if (s.drm_fd < 0) {
    err = "open render node failed";
    return nullptr;
  }

  if (drmSyncobjCreate(s.drm_fd, 0, &s.acquire_syncobj) != 0 ||
      drmSyncobjCreate(s.drm_fd, 0, &s.release_syncobj) != 0) {
    err = "drmSyncobjCreate failed";
    return nullptr;
  }

  // Import both syncobjs into the compositor as timelines. Keep the fds until
  // after a flush so libwayland can send them, then close.
  int afd = -1;
  int rfd = -1;
  if (drmSyncobjHandleToFD(s.drm_fd, s.acquire_syncobj, &afd) != 0 ||
      drmSyncobjHandleToFD(s.drm_fd, s.release_syncobj, &rfd) != 0) {
    if (afd >= 0)
      ::close(afd);
    err = "drmSyncobjHandleToFD failed";
    return nullptr;
  }
  // Timelines have no events → Attach, not SetupHandler.
  wl_proxy* araw =
      wl::construct<wp_linux_drm_syncobj_timeline_v1_traits,
                    mgr::Op::ImportTimeline>(*s.manager.Get(), afd);
  wl_proxy* rraw =
      wl::construct<wp_linux_drm_syncobj_timeline_v1_traits,
                    mgr::Op::ImportTimeline>(*s.manager.Get(), rfd);
  if (araw != nullptr)
    s.acquire_tl.Attach(araw);
  if (rraw != nullptr)
    s.release_tl.Attach(rraw);
  wl_display_flush(display);
  ::close(afd);
  ::close(rfd);
  if (araw == nullptr || rraw == nullptr) {
    err = "import_timeline failed";
    return nullptr;
  }

  // Import the acquire syncobj into Vulkan as a timeline semaphore so a queue
  // submit can signal it (the compositor waits on the same syncobj point).
  ::vk::SemaphoreTypeCreateInfo tci{::vk::SemaphoreType::eTimeline, 0};
  ::vk::SemaphoreCreateInfo sci{};
  sci.setPNext(&tci);
  auto sem_rv = s.device.createSemaphore(sci);
  if (sem_rv.result != ::vk::Result::eSuccess) {
    err = "vkCreateSemaphore (timeline) failed";
    return nullptr;
  }
  s.acquire_sem = sem_rv.value;

  auto import_sem_fd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
      vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
  int vk_afd = -1;
  if (import_sem_fd == nullptr ||
      drmSyncobjHandleToFD(s.drm_fd, s.acquire_syncobj, &vk_afd) != 0) {
    err = "vkImportSemaphoreFdKHR unavailable";
    return nullptr;
  }
  const VkImportSemaphoreFdInfoKHR imp{
      VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      nullptr,
      s.acquire_sem,
      0,  // permanent import: Vulkan takes ownership of vk_afd
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
      vk_afd};
  if (import_sem_fd(device, &imp) != VK_SUCCESS) {
    ::close(vk_afd);
    err = "vkImportSemaphoreFdKHR failed";
    return nullptr;
  }

  // Bind the syncobj surface so we can set acquire/release points per commit.
  wl_proxy* sraw =
      wl::construct<wp_linux_drm_syncobj_surface_v1_traits,
                    mgr::Op::GetSurface>(*s.manager.Get(), surface);
  if (sraw == nullptr) {
    err = "get_surface failed";
    return nullptr;
  }
  s.syncobj_surface.Attach(sraw);
  return es;
}

// ── Per-frame ────────────────────────────────────────────────────────────────

VkSemaphore ExplicitSync::acquire_semaphore() const noexcept {
  return impl_->acquire_sem;
}

ExplicitSync::FramePoints ExplicitSync::NextFrame() noexcept {
  return {++impl_->acquire_point, ++impl_->release_point};
}

void ExplicitSync::SetSurfacePoints(const FramePoints& points) noexcept {
  const auto hi = [](uint64_t v) { return static_cast<uint32_t>(v >> 32u); };
  const auto lo = [](uint64_t v) {
    return static_cast<uint32_t>(v & 0xffffffffu);
  };
  impl_->syncobj_surface.Get()->SetAcquirePoint(
      impl_->acquire_tl.Get()->GetProxy(), hi(points.acquire),
      lo(points.acquire));
  impl_->syncobj_surface.Get()->SetReleasePoint(
      impl_->release_tl.Get()->GetProxy(), hi(points.release),
      lo(points.release));
}

bool ExplicitSync::SlotReleased(uint64_t release_point) const noexcept {
  if (release_point == 0)
    return true;
  uint32_t handle = impl_->release_syncobj;
  uint64_t point = release_point;
  const int r = drmSyncobjTimelineWait(
      impl_->drm_fd, &handle, &point, 1,
      /*timeout_nsec=*/0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, nullptr);
  return r == 0;
}

}  // namespace wl::vk
