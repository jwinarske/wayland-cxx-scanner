// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// skia-vulkan-dmabuf — renders the shared demo scene with Skia's Ganesh Vulkan
// backend and presents it through zwp_linux_dmabuf_v1, so the pixels reach the
// compositor as a dma-buf and never touch wl_shm.
//
// Skia (Ganesh) will only render into GPU-optimal images it owns, not into an
// externally-allocated dma-buf image.  So the pipeline is: Skia draws the scene
// into its own optimal SkSurface, then a vkCmdCopyImage copies that into a
// linear, dma-buf-exported VkImage (VK_KHR_external_memory_fd), which is handed
// to the compositor with DRM_FORMAT_MOD_LINEAR.  The copy is a GPU pass — there
// is no CPU readback and no wl_shm fallback.
//
// This is the linear baseline: single top-level surface, double-buffered,
// CPU-fence synchronized.  Rendering straight into a modifier-tiled exported
// image (single allocation, no copy), an independent subsurface producer, and
// explicit sync (wp_linux_drm_syncobj) are deliberate follow-ups.
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
#include <wl/linux_dmabuf.hpp>  // must follow linux_dmabuf_client.hpp
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

// Confirms the compositor advertises our DRM format on zwp_linux_dmabuf_v1.
class LinuxDmabufHandler
    : public linux_dmabuf_unstable_v1::client::CZwpLinuxDmabufV1<
          LinuxDmabufHandler> {
 public:
  bool has_format = false;
  void OnFormat(uint32_t format) override {
    if (format == kDrmFormat)
      has_format = true;
  }
  void OnModifier(uint32_t format, uint32_t, uint32_t) override {
    if (format == kDrmFormat)
      has_format = true;
  }
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

 private:
  static constexpr int kDefaultWidth = 480;
  static constexpr int kDefaultHeight = 320;
  static constexpr int kMaxDim = 16384;
  static constexpr int kNumSlots = 2;
  static constexpr uint32_t kDmaBufVersion = 3;
  static constexpr int kRoundtripTimeoutMs = 5000;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitVulkan();
  bool
  CreateRenderResources();  // Skia surface + export slots for width_/height_
  void DestroyRenderResources();
  bool CopyToSlot(
      int slot) noexcept;  // Skia image → slot's linear dma-buf image
  bool MainLoop();
  bool RunSelfPaced();
  void PrintBenchmark() const noexcept;
  void PrintPresentSummary() const noexcept;
  void RequestFrameCallback() noexcept;
  void RenderFrame() noexcept;
  bool SaveScreenshot() noexcept;

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

  // Per-slot linear dma-buf image the Skia frame is copied into and presented.
  struct Slot {
    vk::UniqueImage image;
    vk::UniqueDeviceMemory memory;
    vk::DeviceSize mem_size = 0;
    uint32_t stride = 0;
    int dma_fd = -1;
    vk::UniqueCommandBuffer copy_cmd;
    vk::UniqueFence fence;
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

  wl::WlPtr<LinuxDmabufHandler> linux_dmabuf_;
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
  const std::uint32_t dmabuf_ver = std::min(
      {linux_dmabuf_ver_, zwp_linux_dmabuf_v1_traits::version, kDmaBufVersion});
  if (!wl::BindHandler<zwp_linux_dmabuf_v1_traits>(
          registry_, linux_dmabuf_, linux_dmabuf_name_, dmabuf_ver)) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: zwp_linux_dmabuf_v1 bind failed\n");
    return false;
  }
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
  if (!linux_dmabuf_.Get()->has_format) {
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
  }
}

bool App::CreateRenderResources() {
  using namespace linux_dmabuf_unstable_v1::client;
  using namespace wayland::client;

  DestroyRenderResources();

  // Skia-owned optimal render target that the scene draws into.
  skia_surface_ = SkSurfaces::RenderTarget(
      gr_context_.get(), skgpu::Budgeted::kYes,
      SkImageInfo::Make(width_, height_, kSkColorType, kPremul_SkAlphaType),
      /*sampleCount=*/1, kTopLeft_GrSurfaceOrigin, nullptr);
  if (skia_surface_ == nullptr) {
    std::fprintf(stderr,
                 "skia-vulkan-dmabuf: SkSurfaces::RenderTarget failed\n");
    return false;
  }

  const vk::PhysicalDeviceMemoryProperties mem_props =
      vk_.phys_dev.getMemoryProperties();

  // Exportable, linear-tiled copy target: the compositor imports it as
  // DRM_FORMAT_MOD_LINEAR.  Linear is not a valid color attachment on most
  // GPUs, but it is a valid transfer destination everywhere.
  constexpr vk::ExternalMemoryImageCreateInfo ext_img{
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  vk::ImageCreateInfo img_ci;
  img_ci.imageType = vk::ImageType::e2D;
  img_ci.format = kVkFormat;
  img_ci.extent = vk::Extent3D{static_cast<uint32_t>(width_),
                               static_cast<uint32_t>(height_), 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = vk::SampleCountFlagBits::e1;
  img_ci.tiling = vk::ImageTiling::eLinear;
  img_ci.usage = vk::ImageUsageFlagBits::eTransferDst;
  img_ci.sharingMode = vk::SharingMode::eExclusive;
  img_ci.initialLayout = vk::ImageLayout::eUndefined;
  img_ci.setPNext(&ext_img);

  auto get_memory_fd_khr = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
      vkGetDeviceProcAddr(*vk_.device, "vkGetMemoryFdKHR"));
  if (get_memory_fd_khr == nullptr) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: vkGetMemoryFdKHR unavailable\n");
    return false;
  }

  for (Slot& s : slots_) {
    {
      auto rv = vk_.device->createImageUnique(img_ci);
      if (!VkOk(rv.result, "vkCreateImage"))
        return false;
      s.image = std::move(rv.value);
    }
    const vk::MemoryRequirements reqs =
        vk_.device->getImageMemoryRequirements(*s.image);
    s.mem_size = reqs.size;
    s.stride = static_cast<uint32_t>(
        vk_.device
            ->getImageSubresourceLayout(*s.image,
                                        {vk::ImageAspectFlagBits::eColor, 0, 0})
            .rowPitch);

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

    // Export as a dma-buf fd.
    const VkMemoryGetFdInfoKHR fd_info{
        VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, *s.memory,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int raw_fd = -1;
    if (!VkOk(static_cast<vk::Result>(
                  get_memory_fd_khr(*vk_.device, &fd_info, &raw_fd)),
              "vkGetMemoryFdKHR"))
      return false;
    s.dma_fd = raw_fd;

    // Copy command buffer + fence.
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

    // dma-buf-backed wl_buffer (create_immed, linear modifier).
    wl::WlPtr<LinuxBufferParamsHandler> params;
    if (wl_proxy* raw =
            wl::construct<zwp_linux_buffer_params_v1_traits,
                          zwp_linux_dmabuf_v1_traits::Op::CreateParams>(
                *linux_dmabuf_.Get())) {
      params.Attach(raw);
    } else {
      std::fprintf(stderr, "skia-vulkan-dmabuf: create_params failed\n");
      return false;
    }
    params.Get()->Add(s.dma_fd, 0u, 0u, s.stride,
                      static_cast<uint32_t>(
                          static_cast<uint64_t>(DRM_FORMAT_MOD_LINEAR) >> 32u),
                      static_cast<uint32_t>(DRM_FORMAT_MOD_LINEAR));
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
  }

  back_ = 0;
  std::printf(
      "skia-vulkan-dmabuf: %dx%d, %d slots, stride=%u, Ganesh Vulkan → "
      "linear dma-buf present\n",
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
  SkCanvas* canvas = skia_surface_->getCanvas();
  demo::DemoScene::Render(canvas, scene_, view_tree_);
  hud_.Render(canvas, pacer_, fps_.fps());

  if (!screenshot_.empty() && !screenshot_saved_)
    screenshot_saved_ = SaveScreenshot();

  // Finish rendering and hand the Skia image over as a transfer source.
  skgpu::MutableTextureState transfer_src =
      skgpu::MutableTextureStates::MakeVulkan(
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_.queue_family);
  gr_context_->flush(skia_surface_.get(), GrFlushInfo{}, &transfer_src);
  gr_context_->submit(GrSyncCpu::kYes);

  if (!CopyToSlot(back_)) {
    running_ = false;
    return;
  }

  presentation_.Arm(surface_.Get()->GetProxy(), pacer_.frame());
  s.buffer.Get()->released_ = false;
  surface_.Get()->Attach(s.buffer.Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, width_, height_);
  surface_.Get()->Commit();

  back_ = (back_ + 1) % kNumSlots;
  pacer_.Advance();
}

bool App::SaveScreenshot() noexcept {
  SkBitmap bmp;
  if (!bmp.tryAllocPixels(SkImageInfo::Make(width_, height_, kSkColorType,
                                            kPremul_SkAlphaType))) {
    std::fprintf(stderr, "skia-vulkan-dmabuf: screenshot allocPixels failed\n");
    return false;
  }
  if (!skia_surface_->readPixels(bmp, 0, 0)) {
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
