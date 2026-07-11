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

// ── System C headers
// ──────────────────────────────────────────────────────────
extern "C" {
#include <drm_fourcc.h>  // DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR
#include <linux/input-event-codes.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
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
const wl_interface& wl_pointer_traits::wl_iface() noexcept {
  return wl_pointer_interface;
}
const wl_interface& wl_touch_traits::wl_iface() noexcept {
  return wl_touch_interface;
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
  void OnDmabufFeedback(const wl::FeedbackSnapshot& s) { fb_snapshot_ = s; }

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
  // tranche (scanout-first) order, into chosen_modifier_/chosen_plane_count_.
  // Returns false ⇒ use the LINEAR fallback.
  bool ChooseModifier();
  bool AllocateSlotMemory(Slot& s) noexcept;  // memory + dma-buf fd + wl_buffer
  bool CreateModifierSlot(Slot& s) noexcept;  // modifier-tiled + Skia surface
  bool CreateLinearSlot(Slot& s) noexcept;    // linear + copy cmd/fence
  void DestroyRenderResources();
  bool CopyToSlot(
      int slot) noexcept;  // Skia image → slot's linear dma-buf image
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

  bool modifier_ext_ = false;         // modifier extensions enabled on device
  bool use_modifier_ = false;         // direct modifier-tiled path engaged
  uint64_t chosen_modifier_ = 0;      // DRM modifier for the slot images
  uint32_t chosen_plane_count_ = 1;   // memory planes of chosen_modifier_
  wl::FeedbackSnapshot fb_snapshot_;  // latest dmabuf feedback snapshot

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
  for (const vk::PhysicalDevice& pd : phys_rv.value) {
    const vk::PhysicalDeviceType t = pd.getProperties().deviceType;
    if (t == vk::PhysicalDeviceType::eDiscreteGpu ||
        t == vk::PhysicalDeviceType::eIntegratedGpu) {
      vk_.phys_dev = pd;
      break;
    }
  }
  std::printf("skia-vulkan-dmabuf: GPU: %s\n",
              vk_.phys_dev.getProperties().deviceName.data());

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

  // ── Logical device ─────────────────────────────────────────────────────────
  constexpr float queue_prio = 1.0f;
  const vk::DeviceQueueCreateInfo queue_ci{
      {}, vk_.queue_family, 1, &queue_prio};
  const vk::DeviceCreateInfo dev_ci{
      {},
      1,
      &queue_ci,
      0,
      nullptr,
      static_cast<uint32_t>(vk_.device_exts.size()),
      vk_.device_exts.data()};
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
  if (vk_.device)
    (void)vk_.device->waitIdle();
  skia_surface_.reset();
  for (Slot& s : slots_) {
    s.buffer.Reset();
    s.surface.reset();  // borrows s.image; drop before the image
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
  }
}

// Picks a compositor-advertised modifier for kDrmFormat that this GPU can both
// render into and sample, honoring tranche (scanout-first) preference order.
// Returns false when none qualifies — the caller uses the LINEAR fallback.
bool App::ChooseModifier() {
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
        chosen_modifier_ = m;
        chosen_plane_count_ = p->drmFormatModifierPlaneCount;
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

bool App::CreateRenderResources() {
  DestroyRenderResources();

  // SKIA_VULKAN_DMABUF_LINEAR forces the LINEAR+copy fallback (for testing it
  // where the compositor advertises tiled modifiers).
  use_modifier_ = modifier_ext_ &&
                  std::getenv("SKIA_VULKAN_DMABUF_LINEAR") == nullptr &&
                  ChooseModifier();

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
  if (use_modifier_)
    std::printf(
        "skia-vulkan-dmabuf: %dx%d, %d slots, stride=%u, Ganesh Vulkan → "
        "modifier-tiled dma-buf direct (modifier 0x%016llx)\n",
        width_, height_, kNumSlots, slots_.front().stride,
        static_cast<unsigned long long>(chosen_modifier_));
  else
    std::printf(
        "skia-vulkan-dmabuf: %dx%d, %d slots, stride=%u, Ganesh Vulkan → "
        "linear dma-buf present (copy fallback)\n",
        width_, height_, kNumSlots, slots_.front().stride);
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

void App::RenderFrame() noexcept {
  if (needs_resize_) {
    needs_resize_ = false;
    if (!CreateRenderResources()) {
      running_ = false;
      return;
    }
  }

  Slot& s = slots_.at(static_cast<std::size_t>(back_));
  if (!s.buffer.Get()->released_)
    return;  // compositor still holds this slot; drop the frame

  scene_.frame = pacer_.frame();
  fps_.Tick(NowMs());
  view_tree_.Layout(width_, height_);

  // Direct path renders into the slot's own surface; fallback into the shared
  // one that CopyToSlot then blits into the linear slot image.
  SkSurface* target = use_modifier_ ? s.surface.get() : skia_surface_.get();
  SkCanvas* canvas = target->getCanvas();
  demo::DemoScene::Render(canvas, scene_, view_tree_);
  hud_.Render(canvas, pacer_, fps_.fps());

  if (!screenshot_.empty() && !screenshot_saved_)
    screenshot_saved_ = SaveScreenshot(target);

  if (use_modifier_) {
    // Leave the slot image in a compositor-readable layout, then wait for the
    // GPU so the dma-buf holds a complete frame before commit (implicit sync;
    // the CPU wait is what explicit sync would later remove).
    skgpu::MutableTextureState to_present =
        skgpu::MutableTextureStates::MakeVulkan(VK_IMAGE_LAYOUT_GENERAL,
                                                vk_.queue_family);
    gr_context_->flush(target, GrFlushInfo{}, &to_present);
    gr_context_->submit(GrSyncCpu::kYes);
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
  surface_.Get()->Damage(0, 0, width_, height_);
  surface_.Get()->Commit();

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
  if (ev.key == KEY_ESC)
    running_ = false;
  else if (ev.key == KEY_SPACE)
    scene_.button_active = !scene_.button_active;
  else if (ev.key == KEY_F1)
    hud_.toggle();
}

void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED || ev.button != BTN_LEFT)
    return;
  if (view_tree_.HitTest(static_cast<SkScalar>(ev.x),
                         static_cast<SkScalar>(ev.y)) == demo::View::kButton)
    scene_.button_active = !scene_.button_active;
}

void App::OnTouchDown(const wl::TouchPoint& p) noexcept {
  if (view_tree_.HitTest(static_cast<SkScalar>(p.x),
                         static_cast<SkScalar>(p.y)) == demo::View::kButton)
    scene_.button_active = !scene_.button_active;
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
