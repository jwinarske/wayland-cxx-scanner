// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// skia-vulkan-dmabuf — renders the shared demo scene with Skia's Ganesh Vulkan
// backend and presents it through zwp_linux_dmabuf_v1, so the pixels reach the
// compositor as a dma-buf and never touch wl_shm.
//
// Direct path (preferred): wl::DmabufFeedback reports the compositor's
// scanout-capable modifiers; the example picks one this GPU can render into and
// sample (VK_EXT_image_drm_format_modifier), allocates a modifier-tiled,
// dma-buf-exported VkImage per slot, and Skia renders straight into it — no
// copy.  A modifier may span multiple memory planes (e.g. AMD DCC), each
// described to zwp_linux_buffer_params.  Presenting a scanout modifier lets the
// compositor promote the surface onto a hardware plane.
//
// Fallback path: when no renderable+sampleable modifier is on offer (or the
// modifier extensions are absent), Skia renders into its own optimal surface
// and a vkCmdCopyImage blits it into a linear, DRM_FORMAT_MOD_LINEAR exported
// image — the guaranteed baseline.  Force it with SKIA_VULKAN_DMABUF_LINEAR.
//
// Both paths are double-buffered and CPU-fence synchronized; explicit sync
// (wp_linux_drm_syncobj) and buffer-age partial repaint are deliberate
// follow-ups.
//
// Controls:
//   ESC / window close   quit
//   SPACE / left-click    toggles the button-active scene state
//   F1                    toggles the performance overlay (also --hud)

// ── Vulkan C++ bindings
// ───────────────────────────────────────────────────────
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "linux_dmabuf_client.hpp"       // linux_dmabuf_unstable_v1::client
#include "presentation_time_client.hpp"  // presentation_time::client
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"
#if defined(HAVE_DRM_SYNCOBJ)
#include "drm_syncobj_client.hpp"  // linux_drm_syncobj_v1::client
#endif

// ── System C headers
// ──────────────────────────────────────────────────────────
extern "C" {
#include <drm_fourcc.h>  // DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR
#include <linux/input-event-codes.h>
#include <sys/sysmacros.h>  // makedev, major, minor (multi-GPU device match)
#include <sys/types.h>      // dev_t
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#if defined(HAVE_DRM_SYNCOBJ)
#include <fcntl.h>     // open (render node)
#include <sys/stat.h>  // stat, S_ISCHR
#include <xf86drm.h>   // drmSyncobjCreate/HandleToFD/TimelineWait
#endif
}

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/dmabuf_feedback.hpp>  // wl::DmabufFeedback<App>
#include <wl/linux_dmabuf.hpp>     // must follow linux_dmabuf_client.hpp
#include <wl/presentation.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>  // must follow xdg_shell_client.hpp

// ── Shared scene + pacing
// ─────────────────────────────────────────────────────
#include "frame_pacer.hpp"
#include "perf_hud.hpp"
#include "scene.hpp"
#include "view_tree.hpp"

// ── Skia (Ganesh Vulkan)
// ──────────────────────────────────────────────────────
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/MutableTextureState.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#if defined(HAVE_DRM_SYNCOBJ)
#include "include/gpu/ganesh/GrBackendSemaphore.h"
#include "include/gpu/ganesh/vk/GrVkBackendSemaphore.h"
#endif
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "include/gpu/ganesh/vk/GrVkDirectContext.h"
#include "include/gpu/ganesh/vk/GrVkTypes.h"
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "include/gpu/vk/VulkanMutableTextureState.h"
// Concrete memory allocator + ThreadSafe enum live under the installed src/
// tree (no public header exposes them); the prefix is on the -isystem path.
#include "src/gpu/GpuTypesPriv.h"
#include "src/gpu/vk/vulkanmemoryallocator/VulkanMemoryAllocatorPriv.h"

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#if defined(HAVE_DRM_SYNCOBJ)
#include <filesystem>  // resolve the Vulkan device's DRM render node
#endif

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

const wl_interface& wl_callback_traits::wl_iface() noexcept {
  return wl_callback_interface;
}
const wl_interface& wl_compositor_traits::wl_iface() noexcept {
  return wl_compositor_interface;
}
const wl_interface& wl_surface_traits::wl_iface() noexcept {
  return wl_surface_interface;
}
const wl_interface& wl_buffer_traits::wl_iface() noexcept {
  return wl_buffer_interface;
}

}  // namespace wayland::client

namespace {

// DRM_FORMAT_XRGB8888 is memory order B,G,R,X → VkFormat B8G8R8A8Unorm and
// Skia's kBGRA_8888_SkColorType.
constexpr uint32_t kDrmFormat = DRM_FORMAT_XRGB8888;
constexpr vk::Format kVkFormat = vk::Format::eB8G8R8A8Unorm;
constexpr SkColorType kSkColorType = kBGRA_8888_SkColorType;

[[nodiscard]] bool VkOk(vk::Result result, const char* what) noexcept {
  if (result == vk::Result::eSuccess)
    return true;
  std::fprintf(stderr, "skia-vulkan-dmabuf: %s failed (VkResult=%d)\n", what,
               static_cast<int>(result));
  return false;
}

// True if @p pd's DRM render or primary node dev_t equals @p want.  Answers
// only when the device advertises VK_EXT_physical_device_drm (queried before
// device creation, which is allowed for physical-device properties).
[[nodiscard]] bool DeviceMatchesDrm(vk::PhysicalDevice pd, dev_t want) {
  if (want == 0)
    return false;
  auto exts = pd.enumerateDeviceExtensionProperties();
  if (exts.result != vk::Result::eSuccess)
    return false;
  const bool has =
      std::any_of(exts.value.begin(), exts.value.end(),
                  [](const vk::ExtensionProperties& e) {
                    return std::string_view{e.extensionName.data()} ==
                           VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
                  });
  if (!has)
    return false;
  vk::PhysicalDeviceDrmPropertiesEXT drm{};
  vk::PhysicalDeviceProperties2 p2{};
  p2.pNext = &drm;
  pd.getProperties2(&p2);
  return (drm.hasRender != 0u &&
          makedev(static_cast<unsigned>(drm.renderMajor),
                  static_cast<unsigned>(drm.renderMinor)) == want) ||
         (drm.hasPrimary != 0u &&
          makedev(static_cast<unsigned>(drm.primaryMajor),
                  static_cast<unsigned>(drm.primaryMinor)) == want);
}

class App;

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(std::uint32_t time_ms) override;
};

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// Tracks wl_buffer.release so a slot is not re-rendered while the compositor is
// still scanning out of its dma-buf.  released_ starts true.
class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool released_ = true;
  void OnRelease() override { released_ = true; }
};

class LinuxBufferParamsHandler
    : public linux_dmabuf_unstable_v1::client::CZwpLinuxBufferParamsV1<
          LinuxBufferParamsHandler> {};

#if defined(HAVE_DRM_SYNCOBJ)
// The wp_linux_drm_syncobj objects carry no events — bare handlers.
class SyncobjManagerHandler
    : public linux_drm_syncobj_v1::client::CWpLinuxDrmSyncobjManagerV1<
          SyncobjManagerHandler> {};
class SyncobjSurfaceHandler
    : public linux_drm_syncobj_v1::client::CWpLinuxDrmSyncobjSurfaceV1<
          SyncobjSurfaceHandler> {};
class SyncobjTimelineHandler
    : public linux_drm_syncobj_v1::client::CWpLinuxDrmSyncobjTimelineV1<
          SyncobjTimelineHandler> {};
#endif

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  App(demo::PacerConfig pacer_cfg, bool hud, std::string screenshot) noexcept
      : screenshot_(std::move(screenshot)), pacer_(pacer_cfg) {
    hud_.set_visible(hud);
  }
  ~App();

  int Run();

  void OnXdgSurfaceConfigure(std::uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose() { running_ = false; }
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameReady(std::uint32_t time_ms) noexcept;
  void OnPointerButton(const wl::PointerButtonEvent& ev) noexcept;
  void OnTouchDown(const wl::TouchPoint& p) noexcept;
  void OnPresented(const wl::PresentFeedback& fb) noexcept;
  // wl::DmabufFeedback hook: capture the latest snapshot for modifier
  // selection.
  void OnDmabufFeedback(const wl::FeedbackSnapshot& s) {
    fb_snapshot_ = s;
    // A re-advertisement after the slots exist (e.g. the surface's plane
    // assignment changed) may change the best modifier — re-evaluate at the
    // next frame boundary rather than mid-dispatch.
    if (resources_ready_)
      needs_rebuild_ = true;
  }

 private:
  static constexpr int kDefaultWidth = 480;
  static constexpr int kDefaultHeight = 320;
  static constexpr int kMaxDim = 16384;
  static constexpr int kNumSlots = 2;
  static constexpr int kRoundtripTimeoutMs = 5000;

  struct Slot;  // defined below; referenced by the slot helpers

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitVulkan();
  bool
  CreateRenderResources();  // Skia surface + export slots for width_/height_
  // Pick a compositor-advertised modifier that Vulkan can render+sample, in
  // tranche (scanout-first) order.  Side-effect-free (so OnDmabufFeedback can
  // compare); returns false ⇒ use the LINEAR fallback.
  [[nodiscard]] bool ComputeModifier(uint64_t& out_mod,
                                     uint32_t& out_planes) const;
  bool AllocateSlotMemory(Slot& s) noexcept;  // memory + dma-buf fd + wl_buffer
  bool CreateModifierSlot(Slot& s) noexcept;  // modifier-tiled + Skia surface
  bool CreateLinearSlot(Slot& s) noexcept;    // linear + copy cmd/fence
#if defined(HAVE_DRM_SYNCOBJ)
  // Bring up the drm_syncobj ↔ Vulkan-timeline bridge and the syncobj surface;
  // sets use_explicit_sync_ on success.  Called after
  // InitVulkan/CreateSurfaces.
  void SetupExplicitSync() noexcept;
  void DestroyExplicitSync() noexcept;
  // Queue-submit that waits the slot's flush semaphore (Skia's render) and
  // signals the acquire timeline at a fresh point; records the slot's release
  // point.  Returns the acquire point, or 0 on failure.
  uint64_t SignalAcquire(Slot& s) noexcept;
  // True if this slot's compositor release point has been signaled (or the slot
  // was never committed).  Non-blocking poll — the explicit-sync analogue of
  // wl_buffer.release.
  [[nodiscard]] bool SlotReleased(const Slot& s) const noexcept;
#endif
  void DestroyRenderResources();
  bool CopyToSlot(
      int slot) noexcept;  // Skia image → slot's linear dma-buf image
  // Collect this frame's changed rects into damage_ (view + HUD).  Called
  // before render so the region is available for both the clip and the damage.
  void CollectDamage() noexcept;
  // Issue damage_ (or the whole surface on the first frame / resize) between
  // attach and commit.
  void SubmitDamage() noexcept;
  bool MainLoop();
  bool RunSelfPaced();
  void PrintBenchmark() const noexcept;
  void PrintPresentSummary() const noexcept;
  void RequestFrameCallback() noexcept;
  void RenderFrame() noexcept;
  bool SaveScreenshot(SkSurface* surf) noexcept;

  [[nodiscard]] static double NowMs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1.0e6;
  }

  // ── Wayland objects (declaration order = reverse destruction order) ────────
  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlSurfaceHandler> surface_;

  struct VulkanState {
    vk::UniqueInstance instance;    // destroyed LAST
    vk::PhysicalDevice phys_dev{};  // non-owning
    uint32_t queue_family = UINT32_MAX;
    vk::UniqueDevice device;
    vk::Queue queue{};  // non-owning
    vk::UniqueCommandPool cmd_pool;
    std::vector<const char*> device_exts;

    VulkanState() = default;
    VulkanState(const VulkanState&) = delete;
    VulkanState& operator=(const VulkanState&) = delete;
  } vk_;

  skgpu::VulkanExtensions vk_extensions_;
  sk_sp<skgpu::VulkanMemoryAllocator> vk_allocator_;
  sk_sp<GrDirectContext> gr_context_;
  sk_sp<SkSurface> skia_surface_;  // Skia-owned optimal render target

  // Per-slot dma-buf image presented to the compositor.  Direct path: `surface`
  // wraps `image` (a modifier-tiled image Skia renders straight into). Fallback
  // path: `image` is linear and the shared skia_surface_ is copied into it via
  // copy_cmd/fence.
  struct Slot {
    vk::UniqueImage image;
    vk::UniqueDeviceMemory memory;
    vk::DeviceSize mem_size = 0;
    uint32_t stride = 0;  // plane-0 stride, for logging
    struct Plane {
      uint32_t offset = 0;
      uint32_t stride = 0;
    };
    std::array<Plane, 4> planes{};  // one dma-buf, per-plane offset/stride
    uint32_t plane_count = 1;
    int dma_fd = -1;
    vk::UniqueCommandBuffer copy_cmd;  // fallback path only
    vk::UniqueFence fence;             // fallback path only
    sk_sp<SkSurface> surface;          // direct path only (wraps `image`)
    vk::UniqueSemaphore flush_sem;     // explicit-sync: Skia signals on flush
    uint64_t release_point = 0;        // explicit-sync: this slot's release pt
    wl::WlPtr<WlBufferHandler> buffer;

    ~Slot() noexcept {
      if (dma_fd >= 0)
        ::close(dma_fd);
    }
    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
  };
  std::array<Slot, kNumSlots> slots_;
  int back_ = 0;

  wl::DmabufFeedback<App> dmabuf_feedback_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;
  wl::SeatManager<App> seat_;
  wl::PresentationManager<App> presentation_;
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  bool running_ = true;
  bool configured_ = false;
  int width_ = kDefaultWidth;
  int height_ = kDefaultHeight;
  bool needs_resize_ = false;
  bool resources_ready_ = false;  // slots built (so a re-advertisement matters)
  bool needs_rebuild_ = false;    // dmabuf feedback re-advertised → re-evaluate

  bool modifier_ext_ = false;         // modifier extensions enabled on device
  bool use_modifier_ = false;         // direct modifier-tiled path engaged
  uint64_t chosen_modifier_ = 0;      // DRM modifier for the slot images
  uint32_t chosen_plane_count_ = 1;   // memory planes of chosen_modifier_
  wl::FeedbackSnapshot fb_snapshot_;  // latest dmabuf feedback snapshot

  // Explicit sync (wp_linux_drm_syncobj): the compositor waits on our acquire
  // point (GPU-signaled) before sampling and signals the release point when
  // done, so the CPU never blocks on the render.  Gated on protocol + Vulkan
  // support; otherwise the CPU-fence + wl_buffer.release path runs.
  bool use_explicit_sync_ = false;
#if defined(HAVE_DRM_SYNCOBJ)
  bool syncobj_vk_ok_ = false;  // device has timeline + external-semaphore-fd
  std::uint32_t syncobj_mgr_name_ = 0, syncobj_mgr_ver_ = 0;
  wl::WlPtr<SyncobjManagerHandler> syncobj_mgr_;
  wl::WlPtr<SyncobjSurfaceHandler> syncobj_surface_;
  wl::WlPtr<SyncobjTimelineHandler> acquire_tl_;  // wp timeline (acquire)
  wl::WlPtr<SyncobjTimelineHandler> release_tl_;  // wp timeline (release)
  int drm_fd_ = -1;                  // render node, for drmSyncobj* ioctls
  uint32_t acquire_syncobj_ = 0;     // drm handle (Vulkan signals it)
  uint32_t release_syncobj_ = 0;     // drm handle (compositor signals it)
  vk::UniqueSemaphore acquire_sem_;  // Vulkan timeline ↔ acquire_syncobj_
  uint64_t acquire_point_ = 0;       // monotonic acquire timeline value
  uint64_t release_point_ = 0;       // monotonic release timeline value
#endif

  std::uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  std::uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  std::uint32_t linux_dmabuf_name_ = 0, linux_dmabuf_ver_ = 0;

  std::string screenshot_;  // path for --screenshot, empty = disabled
  bool screenshot_saved_ = false;

  demo::SceneState scene_;
  demo::FramePacer pacer_;
  demo::PerfHud hud_;
  demo::FpsMeter fps_;
  demo::ViewTree view_tree_;

  // Honest damage: the compositor is told only the changed region (all paths).
  std::vector<SkIRect> damage_;  // this frame's surface-space damage rects
  int hud_damage_frames_ = 0;    // repaint the HUD region across all slots
  bool full_damage_ = true;      // damage the whole surface on the next commit

  // Buffer-age partial repaint (direct path only): a slot was last rendered
  // kNumSlots frames ago, so re-rendering it needs only the damage accumulated
  // since — slot_accum_[i] is that region (bbox).  The rest is preserved (the
  // spike confirmed Ganesh loads, not clears, a wrapped modifier surface).
  bool use_partial_ = false;  // clip the render to the damage
  std::array<SkIRect, kNumSlots>
      slot_accum_{};                          // damage since slot's last draw
  std::array<bool, kNumSlots> slot_valid_{};  // slot rendered at least once
};

void WlCallbackHandler::OnDone(std::uint32_t time_ms) {
  app_->OnFrameReady(time_ms);
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_running = 1;

extern "C" void OnSigint(int /*signo*/) noexcept {
  g_running = 0;
}

App::~App() {
  if (vk_.device)
    (void)vk_.device->waitIdle();
  DestroyRenderResources();
#if defined(HAVE_DRM_SYNCOBJ)
  DestroyExplicitSync();
#endif
  gr_context_.reset();
  presentation_.Release();
  seat_.Release();
}

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  if (!CreateSurfaces())
    return EXIT_FAILURE;
  if (!InitVulkan())
    return EXIT_FAILURE;
#if defined(HAVE_DRM_SYNCOBJ)
  SetupExplicitSync();
#endif
  if (!CreateRenderResources())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: wl_display_connect failed\n");
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: registry creation failed\n");
    return false;
  }
  registry_.OnGlobal([this](wl::CRegistry&, std::uint32_t name,
                            std::string_view iface, std::uint32_t ver) {
    using namespace wayland::client;
    using namespace xdg_shell::client;
    using dmabuf = linux_dmabuf_unstable_v1::client::zwp_linux_dmabuf_v1_traits;
    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    } else if (iface == presentation_time::client::wp_presentation_traits::
                            interface_name) {
      presentation_.Record(name, ver);
    } else if (iface == dmabuf::interface_name) {
      linux_dmabuf_name_ = name;
      linux_dmabuf_ver_ = ver;
      dmabuf_feedback_.Record(name, ver);
#if defined(HAVE_DRM_SYNCOBJ)
    } else if (iface ==
               linux_drm_syncobj_v1::client::
                   wp_linux_drm_syncobj_manager_v1_traits::interface_name) {
      syncobj_mgr_name_ = name;
      syncobj_mgr_ver_ = ver;
#endif
    }
  });
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: timed out waiting for globals\n");
    return false;
  }
  if (!compositor_name_ || !xdg_wm_base_name_ || !linux_dmabuf_name_) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: compositor/xdg_wm_base/linux-dmabuf not "
                 "advertised\n");
    return false;
  }
  return true;
}

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace linux_dmabuf_unstable_v1::client;

  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "skia-vulkan-dmabuf: wl_compositor bind failed\n");
    return false;
  }
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: xdg_wm_base bind failed\n");
    return false;
  }
  if (!dmabuf_feedback_.Bind(registry_, this)) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: zwp_linux_dmabuf_v1 bind failed\n");
    return false;
  }
  if (dmabuf_feedback_.BoundVersion() >= 4)
    (void)dmabuf_feedback_.StartDefault(display_.Get());
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: wl_seat bind failed\n");
    return false;
  }
  if (!presentation_.Bind(registry_, this)) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: wp_presentation bind failed\n");
    return false;
  }
#if defined(HAVE_DRM_SYNCOBJ)
  // wp_linux_drm_syncobj is optional; explicit sync stays off if absent.  Its
  // objects have no events, so bind via Attach (no listener/_SetProxy).
  if (syncobj_mgr_name_ != 0) {
    using mgr_traits =
        linux_drm_syncobj_v1::client::wp_linux_drm_syncobj_manager_v1_traits;
    if (wl_proxy* raw = registry_.Bind<mgr_traits>(
            syncobj_mgr_name_, std::min(syncobj_mgr_ver_, mgr_traits::version)))
      syncobj_mgr_.Attach(raw);
  }
#endif
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: timed out waiting for formats\n");
    return false;
  }
  if (dmabuf_feedback_.BoundVersion() < 4)
    dmabuf_feedback_.CommitLegacy();
  if (fb_snapshot_.ModifiersFor(kDrmFormat).empty()) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: compositor does not advertise the "
                 "required DRM format\n");
    return false;
  }
  return true;
}

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "skia-vulkan-dmabuf: create_surface failed\n");
    return false;
  }
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: get_toplevel failed\n");
    return false;
  }
  auto* toplevel = xdg_toplevel_.Get();
  toplevel->app_ = this;
  toplevel->SetTitle("skia-vulkan-dmabuf");
  toplevel->SetAppId("org.wayland-cxx.skia-vulkan-dmabuf");

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: timed out waiting for configure\n");
    return false;
  }
  // Per-surface feedback: its tranches reflect this surface's actual
  // plane-assignment potential (the scanout flag), unlike default feedback.
  if (dmabuf_feedback_.BoundVersion() >= 4) {
    (void)dmabuf_feedback_.StartSurface(display_.Get(),
                                        surface_.Get()->GetProxy());
    (void)wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs);
  }
  return true;
}

bool App::InitVulkan() {
  // ── Instance (Vulkan 1.1 — Skia's minimum) ─────────────────────────────────
  constexpr vk::ApplicationInfo app_info{"skia-vulkan-dmabuf",
                                         VK_MAKE_VERSION(0, 1, 0), nullptr, 0,
                                         VK_API_VERSION_1_1};
  const vk::InstanceCreateInfo inst_ci{{}, &app_info};
  auto inst_rv = vk::createInstanceUnique(inst_ci);
  if (!VkOk(inst_rv.result, "vkCreateInstance"))
    return false;
  vk_.instance = std::move(inst_rv.value);

  // ── Physical device (prefer a GPU over a CPU/llvmpipe renderer) ────────────
  auto phys_rv = vk_.instance->enumeratePhysicalDevices();
  if (!VkOk(phys_rv.result, "vkEnumeratePhysicalDevices") ||
      phys_rv.value.empty()) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: no Vulkan physical devices\n");
    return false;
  }
  vk_.phys_dev = phys_rv.value.front();
  // Prefer the GPU the compositor composites on (dmabuf feedback main_device):
  // on a multi-GPU host this allocates on the right device and avoids a silent
  // compositor-side cross-device copy.  Falls back to a discrete/integrated
  // GPU.
  bool matched_device = false;
  for (const vk::PhysicalDevice& pd : phys_rv.value) {
    if (DeviceMatchesDrm(pd, fb_snapshot_.main_device)) {
      vk_.phys_dev = pd;
      matched_device = true;
      break;
    }
  }
  if (!matched_device) {
    for (const vk::PhysicalDevice& pd : phys_rv.value) {
      const vk::PhysicalDeviceType t = pd.getProperties().deviceType;
      if (t == vk::PhysicalDeviceType::eDiscreteGpu ||
          t == vk::PhysicalDeviceType::eIntegratedGpu) {
        vk_.phys_dev = pd;
        break;
      }
    }
  }
  std::printf("skia-vulkan-dmabuf: GPU: %s%s\n",
              vk_.phys_dev.getProperties().deviceName.data(),
              matched_device ? " (matched compositor main_device)" : "");

  // ── Graphics queue family ──────────────────────────────────────────────────
  const auto qfps = vk_.phys_dev.getQueueFamilyProperties();
  for (uint32_t i = 0; i < static_cast<uint32_t>(qfps.size()); ++i) {
    if (qfps.at(i).queueFlags & vk::QueueFlagBits::eGraphics) {
      vk_.queue_family = i;
      break;
    }
  }
  if (vk_.queue_family == UINT32_MAX) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: no graphics queue family\n");
    return false;
  }

  // ── Device extensions: dma-buf export ──────────────────────────────────────
  auto ext_rv = vk_.phys_dev.enumerateDeviceExtensionProperties();
  if (!VkOk(ext_rv.result, "vkEnumerateDeviceExtensionProperties"))
    return false;
  const auto has_ext = [&](std::string_view name) {
    return std::any_of(ext_rv.value.begin(), ext_rv.value.end(),
                       [&](const vk::ExtensionProperties& e) {
                         return std::string_view{e.extensionName.data()} ==
                                name;
                       });
  };
  for (const char* req : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                          VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME}) {
    if (!has_ext(req)) {
      std::fprintf(stderr, "skia-vulkan-dmabuf: device lacks %s\n", req);
      return false;
    }
    vk_.device_exts.push_back(req);
  }
  // Modifier-tiled direct render (no copy) needs these; their absence forces
  // the LINEAR+copy fallback.
  modifier_ext_ = has_ext(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
                  has_ext(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
  if (modifier_ext_) {
    vk_.device_exts.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    vk_.device_exts.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
  }
#if defined(HAVE_DRM_SYNCOBJ)
  // Explicit sync bridges a Vulkan timeline semaphore to a drm_syncobj; needs
  // timeline + external-semaphore-fd, and physical-device-drm to find the node.
  syncobj_vk_ok_ = has_ext(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) &&
                   has_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) &&
                   has_ext(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
  if (syncobj_vk_ok_) {
    vk_.device_exts.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    vk_.device_exts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
    vk_.device_exts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    vk_.device_exts.push_back(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
  }
#endif

  // ── Logical device ─────────────────────────────────────────────────────────
  constexpr float queue_prio = 1.0f;
  const vk::DeviceQueueCreateInfo queue_ci{
      {}, vk_.queue_family, 1, &queue_prio};
  vk::DeviceCreateInfo dev_ci{{},
                              1,
                              &queue_ci,
                              0,
                              nullptr,
                              static_cast<uint32_t>(vk_.device_exts.size()),
                              vk_.device_exts.data()};
#if defined(HAVE_DRM_SYNCOBJ)
  // Timeline semaphores are a feature, not just an extension — enable it.
  vk::PhysicalDeviceTimelineSemaphoreFeatures timeline_feature{VK_TRUE};
  if (syncobj_vk_ok_)
    dev_ci.setPNext(&timeline_feature);
#endif
  auto dev_rv = vk_.phys_dev.createDeviceUnique(dev_ci);
  if (!VkOk(dev_rv.result, "vkCreateDevice"))
    return false;
  vk_.device = std::move(dev_rv.value);
  vk_.queue = vk_.device->getQueue(vk_.queue_family, 0);

  // ── Command pool for the copy passes ───────────────────────────────────────
  {
    const vk::CommandPoolCreateInfo pool_ci{
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer, vk_.queue_family};
    auto rv = vk_.device->createCommandPoolUnique(pool_ci);
    if (!VkOk(rv.result, "vkCreateCommandPool"))
      return false;
    vk_.cmd_pool = std::move(rv.value);
  }

  // ── Skia Ganesh Vulkan context ─────────────────────────────────────────────
  const skgpu::VulkanGetProc get_proc =
      [](const char* name, VkInstance instance,
         VkDevice device) -> PFN_vkVoidFunction {
    if (device != VK_NULL_HANDLE)
      return vkGetDeviceProcAddr(device, name);
    return vkGetInstanceProcAddr(instance, name);
  };
  vk_extensions_.init(get_proc, *vk_.instance, vk_.phys_dev, 0, nullptr,
                      static_cast<uint32_t>(vk_.device_exts.size()),
                      vk_.device_exts.data());

  skgpu::VulkanBackendContext backend{};
  backend.fInstance = *vk_.instance;
  backend.fPhysicalDevice = vk_.phys_dev;
  backend.fDevice = *vk_.device;
  backend.fQueue = vk_.queue;
  backend.fGraphicsQueueIndex = vk_.queue_family;
  backend.fMaxAPIVersion = VK_API_VERSION_1_1;
  backend.fVkExtensions = &vk_extensions_;
  backend.fGetProc = get_proc;
  vk_allocator_ =
      skgpu::VulkanMemoryAllocators::Make(backend, skgpu::ThreadSafe::kNo);
  if (vk_allocator_ == nullptr) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: VulkanMemoryAllocators::Make "
                 "failed\n");
    return false;
  }
  backend.fMemoryAllocator = vk_allocator_;

  gr_context_ = GrDirectContexts::MakeVulkan(backend);
  if (gr_context_ == nullptr) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: GrDirectContexts::MakeVulkan failed\n");
    return false;
  }
  return true;
}

void App::DestroyRenderResources() {
  resources_ready_ = false;
  if (vk_.device)
    (void)vk_.device->waitIdle();
  skia_surface_.reset();
  for (Slot& s : slots_) {
    s.buffer.Reset();
    s.surface.reset();  // borrows s.image; drop before the image
    s.flush_sem.reset();
    s.fence.reset();
    s.copy_cmd.reset();
    s.image.reset();
    s.memory.reset();
    if (s.dma_fd >= 0) {
      ::close(s.dma_fd);
      s.dma_fd = -1;
    }
    s.mem_size = 0;
    s.stride = 0;
    s.plane_count = 1;
    s.planes = {};
    s.release_point = 0;
  }
}

// Picks a compositor-advertised modifier for kDrmFormat that this GPU can both
// render into and sample, honoring tranche (scanout-first) preference order.
// Returns false when none qualifies — the caller uses the LINEAR fallback.
bool App::ComputeModifier(uint64_t& out_mod, uint32_t& out_planes) const {
  vk::DrmFormatModifierPropertiesListEXT list{};
  vk::FormatProperties2 fp2{};
  fp2.pNext = &list;
  vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);
  std::vector<vk::DrmFormatModifierPropertiesEXT> mods(
      list.drmFormatModifierCount);
  list.pDrmFormatModifierProperties = mods.data();
  vk_.phys_dev.getFormatProperties2(kVkFormat, &fp2);

  constexpr auto need = vk::FormatFeatureFlagBits::eColorAttachment |
                        vk::FormatFeatureFlagBits::eSampledImage;
  const auto find =
      [&](uint64_t m) -> const vk::DrmFormatModifierPropertiesEXT* {
    const auto it = std::find_if(
        mods.begin(), mods.end(),
        [&](const vk::DrmFormatModifierPropertiesEXT& p) {
          return p.drmFormatModifier == m &&
                 (p.drmFormatModifierTilingFeatures & need) == need;
        });
    return it == mods.end() ? nullptr : &*it;
  };
  const auto try_list = [&](const std::vector<uint64_t>& cand) {
    for (uint64_t m : cand) {
      if (m == DRM_FORMAT_MOD_INVALID)
        continue;
      if (const vk::DrmFormatModifierPropertiesEXT* p = find(m)) {
        out_mod = m;
        out_planes = p->drmFormatModifierPlaneCount;
        return true;
      }
    }
    return false;
  };
  // Scanout tranches first (plane-promotable), then the rest, tranche order.
  return try_list(fb_snapshot_.ScanoutModifiersFor(kDrmFormat)) ||
         try_list(fb_snapshot_.ModifiersFor(kDrmFormat));
}

// Selects device-local memory, allocates a dedicated exportable block, binds
// it, exports the dma-buf fd, and creates the presented wl_buffer
// (create_params → create_immed) with the slot's tiling modifier.  Requires
// s.image created and s.stride/s.offset filled by the caller.
bool App::AllocateSlotMemory(Slot& s) noexcept {
  using namespace linux_dmabuf_unstable_v1::client;
  using namespace wayland::client;

  const vk::MemoryRequirements reqs =
      vk_.device->getImageMemoryRequirements(*s.image);
  s.mem_size = reqs.size;
  const vk::PhysicalDeviceMemoryProperties mem_props =
      vk_.phys_dev.getMemoryProperties();
  uint32_t mem_type = UINT32_MAX;
  for (uint32_t j = 0; j < mem_props.memoryTypeCount; ++j) {
    if (!(reqs.memoryTypeBits & (1u << j)))
      continue;
    if (mem_props.memoryTypes.at(j).propertyFlags &
        vk::MemoryPropertyFlagBits::eDeviceLocal) {
      mem_type = j;
      break;
    }
    if (mem_type == UINT32_MAX)
      mem_type = j;
  }
  if (mem_type == UINT32_MAX) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: no suitable memory type\n");
    return false;
  }
  vk::MemoryDedicatedAllocateInfo dedicated{*s.image};
  vk::ExportMemoryAllocateInfo export_mem{
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  export_mem.pNext = &dedicated;
  vk::MemoryAllocateInfo mem_ai{reqs.size, mem_type};
  mem_ai.setPNext(&export_mem);
  {
    auto rv = vk_.device->allocateMemoryUnique(mem_ai);
    if (!VkOk(rv.result, "vkAllocateMemory"))
      return false;
    s.memory = std::move(rv.value);
  }
  if (!VkOk(vk_.device->bindImageMemory(*s.image, *s.memory, 0),
            "vkBindImageMemory"))
    return false;

  auto get_memory_fd_khr = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
      vkGetDeviceProcAddr(*vk_.device, "vkGetMemoryFdKHR"));
  if (get_memory_fd_khr == nullptr) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: vkGetMemoryFdKHR unavailable\n");
    return false;
  }
  const VkMemoryGetFdInfoKHR fd_info{
      VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, *s.memory,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  int raw_fd = -1;
  if (!VkOk(static_cast<vk::Result>(
                get_memory_fd_khr(*vk_.device, &fd_info, &raw_fd)),
            "vkGetMemoryFdKHR"))
    return false;
  s.dma_fd = raw_fd;

  const uint64_t mod = use_modifier_
                           ? chosen_modifier_
                           : static_cast<uint64_t>(DRM_FORMAT_MOD_LINEAR);
  wl::WlPtr<LinuxBufferParamsHandler> params;
  if (wl_proxy* raw = dmabuf_feedback_.CreateParams()) {
    params.Attach(raw);
  } else {
    std::fprintf(stderr, "skia-vulkan-dmabuf: create_params failed\n");
    return false;
  }
  // One dma-buf, one add() per memory plane (all share the fd, distinct
  // offsets); libwayland dups the fd for each.
  for (uint32_t i = 0; i < s.plane_count; ++i)
    params.Get()->Add(s.dma_fd, i, s.planes.at(i).offset, s.planes.at(i).stride,
                      static_cast<uint32_t>(mod >> 32u),
                      static_cast<uint32_t>(mod & 0xffffffffu));
  if (wl_proxy* raw =
          wl::construct<wl_buffer_traits,
                        zwp_linux_buffer_params_v1_traits::Op::CreateImmed>(
              *params.Get(), static_cast<int32_t>(width_),
              static_cast<int32_t>(height_), kDrmFormat, 0u)) {
    s.buffer.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "skia-vulkan-dmabuf: create_immed failed\n");
    return false;
  }
  s.buffer.Get()->released_ = true;
  return true;
}

// Direct path: a modifier-tiled, color-attachment+sampled exported image Skia
// renders straight into (no copy).
bool App::CreateModifierSlot(Slot& s) noexcept {
  const std::array<uint64_t, 1> mods{chosen_modifier_};
  vk::ImageDrmFormatModifierListCreateInfoEXT mod_ci{};
  mod_ci.setDrmFormatModifiers(mods);
  vk::ExternalMemoryImageCreateInfo ext_img{
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  ext_img.pNext = &mod_ci;
  vk::ImageCreateInfo ci;
  ci.imageType = vk::ImageType::e2D;
  ci.format = kVkFormat;
  ci.extent = vk::Extent3D{static_cast<uint32_t>(width_),
                           static_cast<uint32_t>(height_), 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = vk::SampleCountFlagBits::e1;
  ci.tiling = vk::ImageTiling::eDrmFormatModifierEXT;
  ci.usage = vk::ImageUsageFlagBits::eColorAttachment |
             vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc |
             vk::ImageUsageFlagBits::eTransferDst;
  ci.sharingMode = vk::SharingMode::eExclusive;
  ci.initialLayout = vk::ImageLayout::eUndefined;
  ci.setPNext(&ext_img);
  {
    auto rv = vk_.device->createImageUnique(ci);
    if (!VkOk(rv.result, "vkCreateImage (modifier)"))
      return false;
    s.image = std::move(rv.value);
  }
  // A DRM modifier may span multiple memory planes (e.g. AMD DCC metadata);
  // query each plane's offset/stride so the wl_buffer describes them all.
  static constexpr std::array<vk::ImageAspectFlagBits, 4> kPlaneAspect = {
      vk::ImageAspectFlagBits::eMemoryPlane0EXT,
      vk::ImageAspectFlagBits::eMemoryPlane1EXT,
      vk::ImageAspectFlagBits::eMemoryPlane2EXT,
      vk::ImageAspectFlagBits::eMemoryPlane3EXT};
  s.plane_count = std::min<uint32_t>(chosen_plane_count_, 4);
  for (uint32_t i = 0; i < s.plane_count; ++i) {
    const vk::SubresourceLayout l = vk_.device->getImageSubresourceLayout(
        *s.image, {kPlaneAspect.at(i), 0, 0});
    s.planes.at(i) = {static_cast<uint32_t>(l.offset),
                      static_cast<uint32_t>(l.rowPitch)};
  }
  s.stride = s.planes[0].stride;
  if (!AllocateSlotMemory(s))
    return false;

  skgpu::VulkanAlloc alloc;
  alloc.fMemory = *s.memory;
  alloc.fOffset = 0;
  alloc.fSize = s.mem_size;
  GrVkImageInfo info;
  info.fImage = *s.image;
  info.fAlloc = alloc;
  info.fImageTiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info.fFormat = static_cast<VkFormat>(kVkFormat);
  info.fImageUsageFlags =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.fSampleCount = 1;
  info.fLevelCount = 1;
  info.fCurrentQueueFamily = vk_.queue_family;
  info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  GrBackendTexture bt = GrBackendTextures::MakeVk(width_, height_, info);
  s.surface = SkSurfaces::WrapBackendTexture(
      gr_context_.get(), bt, kTopLeft_GrSurfaceOrigin,
      /*sampleCnt=*/1, kSkColorType, nullptr, nullptr);
  if (s.surface == nullptr) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: WrapBackendTexture failed\n");
    return false;
  }
#if defined(HAVE_DRM_SYNCOBJ)
  // Binary semaphore Skia signals on flush; the acquire-point submit waits it.
  if (use_explicit_sync_) {
    auto rv = vk_.device->createSemaphoreUnique({});
    if (!VkOk(rv.result, "vkCreateSemaphore (flush)"))
      return false;
    s.flush_sem = std::move(rv.value);
  }
#endif
  return true;
}

// Fallback path: a linear exported image (valid transfer destination
// everywhere) that RenderFrame copies the shared Skia surface into.
bool App::CreateLinearSlot(Slot& s) noexcept {
  constexpr vk::ExternalMemoryImageCreateInfo ext_img{
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  vk::ImageCreateInfo ci;
  ci.imageType = vk::ImageType::e2D;
  ci.format = kVkFormat;
  ci.extent = vk::Extent3D{static_cast<uint32_t>(width_),
                           static_cast<uint32_t>(height_), 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = vk::SampleCountFlagBits::e1;
  ci.tiling = vk::ImageTiling::eLinear;
  ci.usage = vk::ImageUsageFlagBits::eTransferDst;
  ci.sharingMode = vk::SharingMode::eExclusive;
  ci.initialLayout = vk::ImageLayout::eUndefined;
  ci.setPNext(&ext_img);
  {
    auto rv = vk_.device->createImageUnique(ci);
    if (!VkOk(rv.result, "vkCreateImage"))
      return false;
    s.image = std::move(rv.value);
  }
  const vk::SubresourceLayout layout = vk_.device->getImageSubresourceLayout(
      *s.image, {vk::ImageAspectFlagBits::eColor, 0, 0});
  s.plane_count = 1;
  s.planes[0] = {0, static_cast<uint32_t>(layout.rowPitch)};
  s.stride = s.planes[0].stride;
  if (!AllocateSlotMemory(s))
    return false;
  {
    const vk::CommandBufferAllocateInfo cb_ai{
        *vk_.cmd_pool, vk::CommandBufferLevel::ePrimary, 1};
    auto rv = vk_.device->allocateCommandBuffersUnique(cb_ai);
    if (!VkOk(rv.result, "vkAllocateCommandBuffers"))
      return false;
    s.copy_cmd = std::move(rv.value.front());
  }
  {
    auto rv = vk_.device->createFenceUnique({});
    if (!VkOk(rv.result, "vkCreateFence"))
      return false;
    s.fence = std::move(rv.value);
  }
  return true;
}

#if defined(HAVE_DRM_SYNCOBJ)
namespace {
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

// Bring up the drm_syncobj ↔ Vulkan-timeline bridge and the syncobj surface.
// Any failure leaves use_explicit_sync_ false (the CPU-fence path runs).
void App::SetupExplicitSync() noexcept {
  using namespace linux_drm_syncobj_v1::client;
  using mgr = wp_linux_drm_syncobj_manager_v1_traits;

  if (std::getenv("SKIA_VULKAN_DMABUF_NO_EXPLICIT_SYNC") != nullptr)
    return;
  if (syncobj_mgr_.IsNull() || !syncobj_vk_ok_)
    return;

  // Open the Vulkan device's DRM render node for the syncobj ioctls.
  vk::PhysicalDeviceDrmPropertiesEXT drm{};
  vk::PhysicalDeviceProperties2 p2{};
  p2.pNext = &drm;
  vk_.phys_dev.getProperties2(&p2);
  const std::string node =
      drm.hasRender ? DrmNodePath(static_cast<unsigned>(drm.renderMajor),
                                  static_cast<unsigned>(drm.renderMinor))
                    : std::string{};
  if (node.empty())
    return;
  drm_fd_ = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
  if (drm_fd_ < 0)
    return;

  if (drmSyncobjCreate(drm_fd_, 0, &acquire_syncobj_) != 0 ||
      drmSyncobjCreate(drm_fd_, 0, &release_syncobj_) != 0) {
    DestroyExplicitSync();
    return;
  }

  // Import both syncobjs into the compositor as timelines.  Keep the fds until
  // after a flush so libwayland can send them, then close.
  int afd = -1;
  int rfd = -1;
  if (drmSyncobjHandleToFD(drm_fd_, acquire_syncobj_, &afd) != 0 ||
      drmSyncobjHandleToFD(drm_fd_, release_syncobj_, &rfd) != 0) {
    if (afd >= 0)
      ::close(afd);
    DestroyExplicitSync();
    return;
  }
  // Timelines have no events → Attach, not SetupHandler.
  wl_proxy* araw =
      wl::construct<wp_linux_drm_syncobj_timeline_v1_traits,
                    mgr::Op::ImportTimeline>(*syncobj_mgr_.Get(), afd);
  wl_proxy* rraw =
      wl::construct<wp_linux_drm_syncobj_timeline_v1_traits,
                    mgr::Op::ImportTimeline>(*syncobj_mgr_.Get(), rfd);
  if (araw != nullptr)
    acquire_tl_.Attach(araw);
  if (rraw != nullptr)
    release_tl_.Attach(rraw);
  wl_display_flush(display_.Get());
  ::close(afd);
  ::close(rfd);
  if (araw == nullptr || rraw == nullptr) {
    DestroyExplicitSync();
    return;
  }

  // Import the acquire syncobj into Vulkan as a timeline semaphore so a queue
  // submit can signal it (the compositor waits on the same syncobj point).
  vk::SemaphoreTypeCreateInfo tci{vk::SemaphoreType::eTimeline, 0};
  vk::SemaphoreCreateInfo sci{};
  sci.setPNext(&tci);
  auto sem_rv = vk_.device->createSemaphoreUnique(sci);
  if (!VkOk(sem_rv.result, "vkCreateSemaphore (timeline)")) {
    DestroyExplicitSync();
    return;
  }
  acquire_sem_ = std::move(sem_rv.value);

  auto import_sem_fd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
      vkGetDeviceProcAddr(*vk_.device, "vkImportSemaphoreFdKHR"));
  int vk_afd = -1;
  if (import_sem_fd == nullptr ||
      drmSyncobjHandleToFD(drm_fd_, acquire_syncobj_, &vk_afd) != 0) {
    DestroyExplicitSync();
    return;
  }
  const VkImportSemaphoreFdInfoKHR imp{
      VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      nullptr,
      *acquire_sem_,
      0,  // permanent import: Vulkan takes ownership of vk_afd
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
      vk_afd};
  if (import_sem_fd(*vk_.device, &imp) != VK_SUCCESS) {
    ::close(vk_afd);
    DestroyExplicitSync();
    return;
  }

  // Bind the syncobj surface so we can set acquire/release points per commit.
  wl_proxy* sraw = wl::construct<wp_linux_drm_syncobj_surface_v1_traits,
                                 mgr::Op::GetSurface>(
      *syncobj_mgr_.Get(), surface_.Get()->GetProxy());
  if (sraw == nullptr) {
    DestroyExplicitSync();
    return;
  }
  syncobj_surface_.Attach(sraw);
  use_explicit_sync_ = true;
}

void App::DestroyExplicitSync() noexcept {
  use_explicit_sync_ = false;
  if (vk_.device)
    (void)vk_.device->waitIdle();
  acquire_sem_.reset();
  syncobj_surface_.Reset();
  acquire_tl_.Reset();
  release_tl_.Reset();
  if (drm_fd_ >= 0) {
    if (acquire_syncobj_ != 0)
      drmSyncobjDestroy(drm_fd_, acquire_syncobj_);
    if (release_syncobj_ != 0)
      drmSyncobjDestroy(drm_fd_, release_syncobj_);
    ::close(drm_fd_);
    drm_fd_ = -1;
  }
  acquire_syncobj_ = 0;
  release_syncobj_ = 0;
}

uint64_t App::SignalAcquire(Slot& s) noexcept {
  const uint64_t acq = ++acquire_point_;
  s.release_point = ++release_point_;
  const vk::PipelineStageFlags wait_stage =
      vk::PipelineStageFlagBits::eAllCommands;
  uint64_t wait_val = 0;  // ignored for the binary flush semaphore
  vk::TimelineSemaphoreSubmitInfo tl{};
  tl.waitSemaphoreValueCount = 1;
  tl.pWaitSemaphoreValues = &wait_val;
  tl.signalSemaphoreValueCount = 1;
  tl.pSignalSemaphoreValues = &acq;
  vk::SubmitInfo si{};
  si.setWaitSemaphores(*s.flush_sem);
  si.setWaitDstStageMask(wait_stage);
  si.setSignalSemaphores(*acquire_sem_);
  si.setPNext(&tl);
  if (!VkOk(vk_.queue.submit(si), "vkQueueSubmit (acquire)"))
    return 0;
  return acq;
}

// Non-blocking poll of this slot's compositor release point — the explicit-sync
// analogue of wl_buffer.release.  True when the compositor is done reading (or
// the slot has never been committed).
bool App::SlotReleased(const Slot& s) const noexcept {
  if (s.release_point == 0)
    return true;
  uint32_t handle = release_syncobj_;
  uint64_t point = s.release_point;
  const int r =
      drmSyncobjTimelineWait(drm_fd_, &handle, &point, 1, /*timeout_nsec=*/0,
                             DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, nullptr);
  return r == 0;
}
#endif  // HAVE_DRM_SYNCOBJ

bool App::CreateRenderResources() {
  DestroyRenderResources();

  // SKIA_VULKAN_DMABUF_LINEAR forces the LINEAR+copy fallback (for testing it
  // where the compositor advertises tiled modifiers).
  use_modifier_ = modifier_ext_ &&
                  std::getenv("SKIA_VULKAN_DMABUF_LINEAR") == nullptr &&
                  ComputeModifier(chosen_modifier_, chosen_plane_count_);
  // Explicit sync applies only to the direct path; the copy fallback stays
  // CPU-fence synchronized.
  use_explicit_sync_ = use_explicit_sync_ && use_modifier_;
  // Buffer-age partial repaint needs the persistent per-slot surfaces of the
  // direct path.  SKIA_VULKAN_DMABUF_NO_PARTIAL forces full repaint for A/B.
  use_partial_ =
      use_modifier_ && std::getenv("SKIA_VULKAN_DMABUF_NO_PARTIAL") == nullptr;
  slot_valid_.fill(false);  // fresh slots hold no content yet
  slot_accum_.fill(SkIRect::MakeEmpty());

  // Direct path renders into each slot's own persistent surface; the fallback
  // renders into one shared optimal surface and copies it into a linear slot.
  if (!use_modifier_) {
    skia_surface_ = SkSurfaces::RenderTarget(
        gr_context_.get(), skgpu::Budgeted::kYes,
        SkImageInfo::Make(width_, height_, kSkColorType, kPremul_SkAlphaType),
        /*sampleCount=*/1, kTopLeft_GrSurfaceOrigin, nullptr);
    if (skia_surface_ == nullptr) {
      std::fprintf(stderr,
                   "skia-vulkan-dmabuf: SkSurfaces::RenderTarget failed\n");
      return false;
    }
  }

  for (Slot& s : slots_) {
    if (!(use_modifier_ ? CreateModifierSlot(s) : CreateLinearSlot(s)))
      return false;
  }

  back_ = 0;
  const char* sync = use_explicit_sync_ ? "explicit sync" : "CPU fence";
  const char* repaint = use_partial_ ? "partial repaint" : "full repaint";
  if (use_modifier_)
    std::printf(
        "skia-vulkan-dmabuf: %dx%d, %d slots, stride=%u, Ganesh Vulkan → "
        "modifier-tiled dma-buf direct (modifier 0x%016llx, %s, %s)\n",
        width_, height_, kNumSlots, slots_.front().stride,
        static_cast<unsigned long long>(chosen_modifier_), sync, repaint);
  else
    std::printf(
        "skia-vulkan-dmabuf: %dx%d, %d slots, stride=%u, Ganesh Vulkan → "
        "linear dma-buf present (copy fallback, %s)\n",
        width_, height_, kNumSlots, slots_.front().stride, sync);
  resources_ready_ = true;
  return true;
}

// Copies the just-rendered Skia image into the given slot's linear dma-buf
// image and waits for the GPU to finish, so the compositor reads a complete
// frame.  The Skia image was left in TRANSFER_SRC_OPTIMAL by RenderFrame().
bool App::CopyToSlot(int slot) noexcept {
  Slot& s = slots_.at(static_cast<std::size_t>(slot));

  GrBackendTexture bt = SkSurfaces::GetBackendTexture(
      skia_surface_.get(), SkSurfaces::BackendHandleAccess::kFlushRead);
  GrVkImageInfo src_info{};
  if (!GrBackendTextures::GetVkImageInfo(bt, &src_info)) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: GetVkImageInfo failed\n");
    return false;
  }
  const vk::Image src_image{src_info.fImage};
  const auto src_layout = static_cast<vk::ImageLayout>(src_info.fImageLayout);

  const vk::CommandBuffer cmd = *s.copy_cmd;
  (void)cmd.reset();
  (void)cmd.begin(vk::CommandBufferBeginInfo{
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

  const vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1,
                                        0, 1};
  // Prepare the linear dma-buf image as the copy destination.
  const vk::ImageMemoryBarrier to_dst{{},
                                      vk::AccessFlagBits::eTransferWrite,
                                      vk::ImageLayout::eUndefined,
                                      vk::ImageLayout::eGeneral,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *s.image,
                                      color};
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                      vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_dst);

  const vk::ImageCopy region{
      {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
      {0, 0, 0},
      {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
      {0, 0, 0},
      {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1}};
  cmd.copyImage(src_image, src_layout, *s.image, vk::ImageLayout::eGeneral,
                region);

  // Make the copy visible to the external (dma-buf) reader.
  const vk::ImageMemoryBarrier to_ext{vk::AccessFlagBits::eTransferWrite,
                                      {},
                                      vk::ImageLayout::eGeneral,
                                      vk::ImageLayout::eGeneral,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      VK_QUEUE_FAMILY_IGNORED,
                                      *s.image,
                                      color};
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                      vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {},
                      to_ext);
  (void)cmd.end();

  vk::SubmitInfo submit{};
  submit.setCommandBuffers(cmd);
  if (!VkOk(vk_.queue.submit(submit, *s.fence), "vkQueueSubmit (copy)"))
    return false;
  if (!VkOk(vk_.device->waitForFences(*s.fence, VK_TRUE, UINT64_MAX),
            "vkWaitForFences (copy)"))
    return false;
  (void)vk_.device->resetFences(*s.fence);
  return true;
}

void App::CollectDamage() noexcept {
  damage_.clear();
  view_tree_.CollectDamage(damage_);
  // The HUD updates every frame while visible; keep its region damaged for a
  // few frames after a visibility change so every slot is refreshed.
  if (hud_.visible()) {
    damage_.push_back(hud_.Bounds());
  } else if (hud_damage_frames_ > 0) {
    damage_.push_back(hud_.Bounds());
    --hud_damage_frames_;
  }
}

// Damage the surface with the frame's changed rects, or the whole surface on
// the first frame / after a resize.  The scene coordinate space equals the
// surface space here (no fractional scale).  On the direct path the render was
// clipped to a larger (buffer-age) region, but only these rects actually
// changed since the previous frame, so this is the correct compositor damage.
void App::SubmitDamage() noexcept {
  auto* surface = surface_.Get();
  if (full_damage_) {
    surface->Damage(0, 0, width_, height_);
    full_damage_ = false;
    return;
  }
  for (const SkIRect& r : damage_)
    surface->Damage(r.x(), r.y(), r.width(), r.height());
}

void App::RenderFrame() noexcept {
  if (needs_resize_) {
    needs_resize_ = false;
    full_damage_ = true;  // fresh slots → damage everything next commit
    if (!CreateRenderResources()) {
      running_ = false;
      return;
    }
  }
  if (needs_rebuild_) {
    needs_rebuild_ = false;
    // The compositor re-advertised feedback.  Rebuild the slot ring only when
    // it actually changes the direct-path modifier (e.g. the surface became
    // plane-promotable) — a plain re-advertisement with the same modifier keeps
    // the slots.  Downgrades to/from the fallback are left to the next resize.
    uint64_t mod = 0;
    uint32_t planes = 1;
    if (use_modifier_ && ComputeModifier(mod, planes) &&
        mod != chosen_modifier_) {
      std::printf(
          "skia-vulkan-dmabuf: dmabuf feedback re-advertised → rebuilding "
          "slots "
          "(modifier 0x%016llx)\n",
          static_cast<unsigned long long>(mod));
      full_damage_ = true;
      if (!CreateRenderResources()) {
        running_ = false;
        return;
      }
    }
  }

  Slot& s = slots_.at(static_cast<std::size_t>(back_));
  // Compositor still reading this slot? Drop the frame.  Explicit sync polls
  // the release point; otherwise the wl_buffer.release flag.
#if defined(HAVE_DRM_SYNCOBJ)
  if (use_explicit_sync_ ? !SlotReleased(s) : !s.buffer.Get()->released_)
    return;
#else
  if (!s.buffer.Get()->released_)
    return;
#endif

  scene_.frame = pacer_.frame();
  fps_.Tick(NowMs());
  view_tree_.Layout(width_, height_);
  view_tree_.MarkDirty(demo::View::kSpinner);  // the arc animates every frame
  CollectDamage();                             // this frame's changed rects

  // Direct path renders into the slot's own surface; fallback into the shared
  // one that CopyToSlot then blits into the linear slot image.
  SkSurface* target = use_modifier_ ? s.surface.get() : skia_surface_.get();
  SkCanvas* canvas = target->getCanvas();

  // Buffer-age partial repaint (direct path): re-render only the region that
  // changed since this slot was last drawn — its own accumulated damage plus
  // this frame's — trusting Ganesh to preserve the rest of the persistent slot.
  const bool clip = use_partial_;
  const auto bi = static_cast<std::size_t>(back_);
  if (clip) {
    SkIRect redraw;
    if (!slot_valid_.at(bi)) {
      redraw = SkIRect::MakeWH(width_, height_);  // first use: full
    } else {
      redraw = slot_accum_.at(bi);
      for (const SkIRect& r : damage_)
        redraw.join(r);
    }
    canvas->save();
    canvas->clipIRect(redraw);
  }
  demo::DemoScene::Render(canvas, scene_, view_tree_);
  hud_.Render(canvas, pacer_, fps_.fps());
  if (clip) {
    canvas->restore();
    // This slot is now current; every other slot accrues this frame's damage.
    for (std::size_t j = 0; j < static_cast<std::size_t>(kNumSlots); ++j)
      if (j != bi)
        for (const SkIRect& r : damage_)
          slot_accum_.at(j).join(r);
    slot_accum_.at(bi).setEmpty();
    slot_valid_.at(bi) = true;
  }

  if (!screenshot_.empty() && !screenshot_saved_)
    screenshot_saved_ = SaveScreenshot(target);

  uint64_t acquire_pt = 0;  // explicit-sync acquire point for this commit
  (void)acquire_pt;
  if (use_modifier_) {
    // Leave the slot image in a compositor-readable layout.
    skgpu::MutableTextureState to_present =
        skgpu::MutableTextureStates::MakeVulkan(VK_IMAGE_LAYOUT_GENERAL,
                                                vk_.queue_family);
#if defined(HAVE_DRM_SYNCOBJ)
    if (use_explicit_sync_) {
      // Skia signals a binary semaphore on flush (no CPU wait); a queue submit
      // waits it and signals the acquire timeline point the compositor waits
      // on.
      GrBackendSemaphore backend_sem =
          GrBackendSemaphores::MakeVk(*s.flush_sem);
      GrFlushInfo fi;
      fi.fNumSemaphores = 1;
      fi.fSignalSemaphores = &backend_sem;
      gr_context_->flush(target, fi, &to_present);
      gr_context_->submit(GrSyncCpu::kNo);
      acquire_pt = SignalAcquire(s);
      if (acquire_pt == 0) {
        running_ = false;
        return;
      }
    } else {
      // No explicit sync: wait for the GPU so the dma-buf holds a complete
      // frame before commit (implicit dma-buf sync).
      gr_context_->flush(target, GrFlushInfo{}, &to_present);
      gr_context_->submit(GrSyncCpu::kYes);
    }
#else
    gr_context_->flush(target, GrFlushInfo{}, &to_present);
    gr_context_->submit(GrSyncCpu::kYes);
#endif
  } else {
    // Finish rendering and hand the Skia image over as a transfer source.
    skgpu::MutableTextureState transfer_src =
        skgpu::MutableTextureStates::MakeVulkan(
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_.queue_family);
    gr_context_->flush(target, GrFlushInfo{}, &transfer_src);
    gr_context_->submit(GrSyncCpu::kYes);
    if (!CopyToSlot(back_)) {
      running_ = false;
      return;
    }
  }

  presentation_.Arm(surface_.Get()->GetProxy(), pacer_.frame());
  s.buffer.Get()->released_ = false;
  surface_.Get()->Attach(s.buffer.Get()->GetProxy(), 0, 0);
  SubmitDamage();
#if defined(HAVE_DRM_SYNCOBJ)
  if (use_explicit_sync_) {
    // Points are per-commit and must be set between attach and commit.
    const auto hi = [](uint64_t v) { return static_cast<uint32_t>(v >> 32u); };
    const auto lo = [](uint64_t v) {
      return static_cast<uint32_t>(v & 0xffffffffu);
    };
    syncobj_surface_.Get()->SetAcquirePoint(acquire_tl_.Get()->GetProxy(),
                                            hi(acquire_pt), lo(acquire_pt));
    syncobj_surface_.Get()->SetReleasePoint(release_tl_.Get()->GetProxy(),
                                            hi(s.release_point),
                                            lo(s.release_point));
  }
#endif
  surface_.Get()->Commit();

  view_tree_.ClearDirty();
  back_ = (back_ + 1) % kNumSlots;
  pacer_.Advance();
}

bool App::SaveScreenshot(SkSurface* surf) noexcept {
  SkBitmap bmp;
  if (!bmp.tryAllocPixels(SkImageInfo::Make(width_, height_, kSkColorType,
                                            kPremul_SkAlphaType))) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: screenshot allocPixels failed\n");
    return false;
  }
  if (!surf->readPixels(bmp, 0, 0)) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: readPixels failed\n");
    return false;
  }
  SkFILEWStream out(screenshot_.c_str());
  if (!out.isValid() ||
      !SkPngEncoder::Encode(&out, bmp.pixmap(), SkPngEncoder::Options{})) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: PNG encode to %s failed\n",
                 screenshot_.c_str());
    return false;
  }
  std::printf("skia-vulkan-dmabuf: wrote screenshot %s\n", screenshot_.c_str());
  return true;
}

void App::OnPresented(const wl::PresentFeedback& fb) noexcept {
  pacer_.RecordPresentMs(fb.latency_ms);
  pacer_.NoteRefreshNs(fb.refresh_ns);
}

bool App::RunSelfPaced() {
  std::printf("skia-vulkan-dmabuf: self-paced run%s\n",
              pacer_.benchmarking() ? " (benchmark)" : "");
  while (running_ && g_running && !pacer_.reached_limit()) {
    const double t0 = NowMs();
    RenderFrame();
    const double render_ms = NowMs() - t0;
    if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
      std::fprintf(stderr, "skia-vulkan-dmabuf: roundtrip failed\n");
      return false;
    }
    pacer_.RecordFrameMs(render_ms);
  }
  if (pacer_.benchmarking())
    PrintBenchmark();
  return true;
}

void App::PrintBenchmark() const noexcept {
  std::printf(
      "skia-vulkan-dmabuf: %zu frames  mean=%.3f ms  p50=%.3f  p95=%.3f  "
      "p99=%.3f\n",
      pacer_.sample_count(), pacer_.Mean(), pacer_.Percentile(50),
      pacer_.Percentile(95), pacer_.Percentile(99));
  PrintPresentSummary();
}

void App::PrintPresentSummary() const noexcept {
  if (pacer_.present_count() == 0)
    return;
  std::printf(
      "skia-vulkan-dmabuf: presentation: %llu shown  latency mean=%.3f ms  "
      "p50=%.3f  p95=%.3f  refresh=%.2f Hz\n",
      static_cast<unsigned long long>(pacer_.present_count()),
      pacer_.PresentMean(), pacer_.PresentPercentile(50),
      pacer_.PresentPercentile(95), pacer_.refresh_hz());
}

bool App::MainLoop() {
  if (pacer_.self_paced())
    return RunSelfPaced();

  std::printf("skia-vulkan-dmabuf: press ESC or Ctrl-C to quit\n");
  RequestFrameCallback();
  RenderFrame();
  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; },
      "skia-vulkan-dmabuf");
  PrintPresentSummary();
  return ok;
}

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_callback_.Get()->app_ = this;
    frame_callback_.Get()->_SetProxy(raw);
  }
}

void App::OnFrameReady(std::uint32_t /*time_ms*/) noexcept {
  wl_proxy* const spent = frame_callback_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent != nullptr)
      wl_proxy_destroy(spent);
  }};
  RequestFrameCallback();
  RenderFrame();
}

void App::OnXdgSurfaceConfigure(std::uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(int32_t w, int32_t h) {
  if (w > 0 && h > 0) {
    const int nw = std::min(w, kMaxDim);
    const int nh = std::min(h, kMaxDim);
    if (nw != width_ || nh != height_) {
      width_ = nw;
      height_ = nh;
      needs_resize_ = true;
    }
  }
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;
  if (ev.key == KEY_ESC) {
    running_ = false;
  } else if (ev.key == KEY_SPACE) {
    scene_.button_active = !scene_.button_active;
    view_tree_.MarkDirty(demo::View::kButton);
  } else if (ev.key == KEY_F1) {
    hud_.toggle();
    hud_damage_frames_ = kNumSlots;  // clear/paint the HUD in every slot
  }
}

void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED || ev.button != BTN_LEFT)
    return;
  if (view_tree_.HitTest(static_cast<SkScalar>(ev.x),
                         static_cast<SkScalar>(ev.y)) == demo::View::kButton) {
    scene_.button_active = !scene_.button_active;
    view_tree_.MarkDirty(demo::View::kButton);
  }
}

void App::OnTouchDown(const wl::TouchPoint& p) noexcept {
  if (view_tree_.HitTest(static_cast<SkScalar>(p.x),
                         static_cast<SkScalar>(p.y)) == demo::View::kButton) {
    scene_.button_active = !scene_.button_active;
    view_tree_.MarkDirty(demo::View::kButton);
  }
}

}  // namespace

namespace {

void PrintUsage() {
  std::printf(
      "usage: skia_vulkan_dmabuf [--frames N] [--exit] [--fixed-dt]\n"
      "                          [--benchmark N] [--hud] [--screenshot FILE]\n"
      "  --frames N        render at most N frames\n"
      "  --exit            quit once the frame limit is reached\n"
      "  --fixed-dt        deterministic 60 Hz animation clock\n"
      "  --benchmark N     render N frames self-paced and print frame-time "
      "stats\n"
      "  --hud             show the performance overlay (toggle with F1)\n"
      "  --screenshot FILE read the first rendered frame back to a PNG "
      "(verifies\n"
      "                    the render path without a compositor grab)\n");
}

[[nodiscard]] bool ParseArgs(const std::vector<std::string_view>& args,
                             demo::PacerConfig& cfg,
                             bool& hud,
                             std::string& screenshot) {
  constexpr long kMaxFrames = 1'000'000;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    const auto next_int = [&](int& out) {
      if (i + 1 >= args.size())
        return false;
      const std::string s(args[i + 1]);
      char* end = nullptr;
      const long value = std::strtol(s.c_str(), &end, 10);
      if (end == s.c_str() || value < 1 || value > kMaxFrames)
        return false;
      out = static_cast<int>(value);
      ++i;
      return true;
    };
    if (a == "--frames") {
      if (!next_int(cfg.max_frames))
        return false;
    } else if (a == "--exit") {
      cfg.exit_on_limit = true;
    } else if (a == "--fixed-dt") {
      cfg.fixed_dt = true;
    } else if (a == "--benchmark") {
      if (!next_int(cfg.max_frames))
        return false;
      cfg.benchmark = true;
      cfg.exit_on_limit = true;
    } else if (a == "--hud") {
      hud = true;
    } else if (a == "--screenshot") {
      if (i + 1 >= args.size())
        return false;
      screenshot = std::string(args[++i]);
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, OnSigint);

  const std::vector<std::string_view> args(argv, std::next(argv, argc));
  demo::PacerConfig cfg;
  bool hud = false;
  std::string screenshot;
  if (!ParseArgs(args, cfg, hud, screenshot)) {
    PrintUsage();
    return EXIT_FAILURE;
  }

  App app(cfg, hud, std::move(screenshot));
  return app.Run();
}
