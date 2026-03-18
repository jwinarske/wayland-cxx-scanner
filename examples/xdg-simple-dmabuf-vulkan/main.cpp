// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// xdg-simple-dmabuf-vulkan — Vulkan DMA-BUF Wayland client.
//
// Connects to the running compositor, creates an XDG toplevel window, and
// renders a hue-shifting solid colour into a DMA-BUF-backed Vulkan image
// exported directly to the compositor via the linux-dmabuf-unstable-v1
// protocol (version 3).  The image lives in Vulkan device memory; a single
// DMA-BUF fd is exported at start-up and re-used for every frame.
//
// Rendering per frame:
//   1. Record vkCmdClearColorImage with a cycling hue into the VkImage.
//   2. Submit to the graphics queue and wait on a VkFence (synchronous).
//   3. Arm a wl_surface.frame callback, attach the wl_buffer backed by the
//      DMA-BUF, damage the full surface, and commit — all in one message
//      batch to the compositor.
//
// Build requirements:
//   wayland-client, vulkan (≥ 1.1 + VK_KHR_external_memory_fd),
//   wayland-protocols (linux-dmabuf-unstable-v1), libdrm, xkbcommon.
// Runtime requirement:
//   A Wayland compositor with xdg-shell and zwp_linux_dmabuf_v1 ≥ 3.

// ── Vulkan C++ bindings ───────────────────────────────────────────────────────
// VULKAN_HPP_NO_EXCEPTIONS: every Vulkan call returns vk::Result /
// vk::ResultValue<T> rather than throwing; every code path is explicit.
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>

// ── Generated C++ protocol headers ───────────────────────────────────────────
// linux_dmabuf_client.hpp  → namespace linux_dmabuf_unstable_v1::client
// wayland_client.hpp       → namespace wayland::client
// xdg_shell_client.hpp     → namespace xdg_shell::client
#include "linux_dmabuf_client.hpp"
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── System C headers ──────────────────────────────────────────────────────────
extern "C" {
// DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR.
#include <drm_fourcc.h>
// KEY_ESC, WL_KEYBOARD_KEY_STATE_PRESSED.
#include <linux/input-event-codes.h>
// close().
#include <unistd.h>
// wl_*_interface symbols and WL_SEAT_CAPABILITY_KEYBOARD.
#include <wayland-client-protocol.h>
}

// ── Framework headers ─────────────────────────────────────────────────────────
// SetupHandler(), BindHandler(), RunEventLoop().
#include <wl/client_helpers.hpp>
// DisplayHandle, RoundtripWithTimeout().
#include <wl/display.hpp>
// wl::dmabuf wl_interface tables + wl_iface() impls for linux-dmabuf.
// Must follow linux_dmabuf_client.hpp.
#include <wl/linux_dmabuf.hpp>
// CRegistry.
#include <wl/registry.hpp>
// SeatManager<App> (wl_seat + wl_keyboard + xkbcommon).
#include <wl/seat.hpp>
// WlPtr<T>.
#include <wl/wl_ptr.hpp>
// XDG interface tables + CRTP handlers.  Must follow xdg_shell_client.hpp.
#include <wl/xdg_shell.hpp>

// ── Standard library ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>  // std::data, std::size

// ── POSIX ─────────────────────────────────────────────────────────────────────
#include <poll.h>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// <wayland-client-protocol.h> exposes pre-built extern const wl_interface
// symbols for every core Wayland interface.
// wl_seat_traits::wl_iface() and wl_keyboard_traits::wl_iface() are provided
// inline by <wl/seat.hpp> (already included above).
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

// xdg-shell wl_interface tables and wl_iface() implementations are provided
// by <wl/xdg_shell.hpp> (already included above).
// linux-dmabuf wl_interface tables and wl_iface() implementations are
// provided by <wl/linux_dmabuf.hpp> (already included above).

// ══════════════════════════════════════════════════════════════════════════════
// Vulkan result helper
// ══════════════════════════════════════════════════════════════════════════════

namespace {

/// Log a Vulkan error to stderr and return false.  Returns true on success.
bool VkOk(vk::Result result, const char* what) noexcept {
  if (result == vk::Result::eSuccess)
    return true;
  std::fprintf(stderr,
               "xdg-simple-dmabuf-vulkan: %s failed (VkResult=%d)\n", what,
               static_cast<int>(result));
  return false;
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
//
// Each handler inherits the generated CRTP base and overrides only the virtual
// event methods it cares about.  Constructor requests (get_xdg_surface,
// get_toplevel, wl_surface.frame, create_params, create_immed, …) are issued
// at the call site using wl::construct<ChildTraits, Opcode>(parent, args…).
//
// wl::XdgWmBaseHandler, wl::XdgSurfaceHandler<App>, wl::XdgToplevelHandler<App>
// are provided by <wl/xdg_shell.hpp>.
// wl::SeatManager<App> (seat + keyboard) is provided by <wl/seat.hpp>.
// ══════════════════════════════════════════════════════════════════════════════

// Forward-declare App so handler callbacks can call back into it.
class App;

// ── WlCallbackHandler ────────────────────────────────────────────────────────
// Handles the one-shot wl_callback.done event emitted by the compositor to
// pace frame production.  A new instance (proxy) is allocated for every frame
// via wl_surface.frame; after done fires, the proxy is destroyed.

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;

  void OnDone(uint32_t time_ms) override;
};

// ── WlCompositorHandler ──────────────────────────────────────────────────────
// wl_compositor has no events.  Provide the required ProcessEvent stub so the
// class is concrete.  We attach via WlPtr::Attach() rather than _SetProxy() so
// no listener table is needed.

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlSurfaceHandler ─────────────────────────────────────────────────────────
// Minimal wl_surface handler.  CWlSurface already provides Destroy() and
// default no-op overrides for enter/leave/preferred_buffer_*.

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// ── WlBufferHandler ──────────────────────────────────────────────────────────
// Tracks wl_buffer.release so we know when the compositor is done reading the
// DMA-BUF.  released_ starts true so the very first RenderFrame() proceeds.

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool released_ = true;
  void OnRelease() override { released_ = true; }
};

// ── LinuxDmabufHandler ───────────────────────────────────────────────────────
// Receives format/modifier advertisements from zwp_linux_dmabuf_v1 and checks
// that the compositor supports DRM_FORMAT_ARGB8888.

class LinuxDmabufHandler
    : public linux_dmabuf_unstable_v1::client::CZwpLinuxDmabufV1<
          LinuxDmabufHandler> {
 public:
  bool has_argb8888 = false;

  void OnFormat(uint32_t format) override {
    if (format == DRM_FORMAT_ARGB8888)
      has_argb8888 = true;
  }
  void OnModifier(uint32_t format, uint32_t /*mod_hi*/,
                  uint32_t /*mod_lo*/) override {
    if (format == DRM_FORMAT_ARGB8888)
      has_argb8888 = true;
  }
};

// ── LinuxBufferParamsHandler ─────────────────────────────────────────────────
// Used transiently inside CreateDmaBufBuffer() to build a wl_buffer via the
// synchronous create_immed path.  The created/failed events are not needed;
// the default no-op overrides in the generated base are sufficient.

class LinuxBufferParamsHandler
    : public linux_dmabuf_unstable_v1::client::CZwpLinuxBufferParamsV1<
          LinuxBufferParamsHandler> {};

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks invoked by the CRTP handlers ──────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose();
  void OnKey(uint32_t key, uint32_t state);
  /// Called by WlCallbackHandler::OnDone — render one frame and arm the next
  /// frame callback.
  void OnFrameReady(uint32_t time_ms) noexcept;

 private:
  // ── Member declaration order determines RAII destruction order.
  //    Declared first → destroyed last; declared last → destroyed first.
  //
  //    Destruction sequence (reverse of declaration order):
  //      frame_callback_ → seat_ (keyboard_ first, then seat_ inside) →
  //      xdg_toplevel_ → xdg_surface_ → xdg_wm_base_ →
  //      wl_buffer_ → linux_dmabuf_ → vk_ (VulkanState dtor) →
  //      surface_ → compositor_ → registry_ → display_

  // Wayland display — destroyed last so all proxy operations remain valid.
  wl::DisplayHandle display_;

  // Registry — destroyed before display_.
  wl::CRegistry registry_;

  // wl_compositor — no events; WlPtr::Attach() used (no s_listener_table_).
  wl::WlPtr<WlCompositorHandler> compositor_;

  // wl_surface — CWlSurface::Destroy() sends destroy request before proxy drop.
  wl::WlPtr<WlSurfaceHandler> surface_;

  // Vulkan state — declared after surface_ so VulkanState::~VulkanState() runs
  // (closing the DMA-BUF fd and releasing all Vulkan objects) before the
  // wl_surface proxy is destroyed.
  //
  // Member declaration order inside VulkanState is chosen so that
  // vk::UniqueX handles destroy in the correct Vulkan teardown order
  // (reverse of declaration = reverse of creation):
  //   fence → memory → image → cmd_buf → cmd_pool → device → instance
  struct VulkanState {
    vk::UniqueInstance instance;    // destroyed LAST (after everything)
    vk::PhysicalDevice phys_dev{};  // non-owning handle
    uint32_t queue_family = UINT32_MAX;
    vk::UniqueDevice device;   // destroyed before instance
    vk::Queue queue{};         // non-owning handle
    vk::UniqueCommandPool cmd_pool;    // destroyed before device
    vk::UniqueCommandBuffer cmd_buf;   // destroyed before cmd_pool (freed)
    vk::UniqueImage image;             // destroyed before device
    vk::UniqueDeviceMemory memory;     // destroyed before device
    vk::UniqueFence fence;             // destroyed FIRST (no dependants)
    int dma_fd = -1;
    uint32_t stride = 0;  // bytes per row; set by getImageSubresourceLayout

    ~VulkanState() noexcept {
      // Close the exported fd before the Vulkan memory object is released.
      if (dma_fd >= 0) {
        ::close(dma_fd);
        dma_fd = -1;
      }
      // vk::UniqueX members destroy automatically in reverse declaration order.
    }
    VulkanState() = default;
    VulkanState(const VulkanState&) = delete;
    VulkanState& operator=(const VulkanState&) = delete;
  } vk_;

  // linux_dmabuf_ — destroyed before vk_ so the compositor is told we are done
  // with the dmabuf global before the Vulkan memory is freed.
  wl::WlPtr<LinuxDmabufHandler> linux_dmabuf_;

  // wl_buffer_ — destroyed before linux_dmabuf_; sends wl_buffer.destroy to
  // the compositor so it can release its reference to the DMA-BUF.
  wl::WlPtr<WlBufferHandler> wl_buffer_;

  // XDG CRTP handlers — destroyed in reverse: toplevel first, wm_base last.
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // Seat + keyboard manager — keyboard_ inside is destroyed before seat_.
  wl::SeatManager<App> seat_;

  // Frame-pacing callback — destroyed first among all WlPtrs.
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // Application state.
  bool running_ = true;
  bool configured_ = false;
  // Guards OnToplevelConfigure: once Vulkan is initialised the buffer size is
  // fixed and resize requests are ignored.
  bool vulkan_init_ = false;
  int width_ = 800;
  int height_ = 600;
  uint64_t frame_ = 0;

  // Global ids recorded during the registry scan.
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t linux_dmabuf_name_ = 0, linux_dmabuf_ver_ = 0;

  /// Maximum time (ms) to wait for a compositor response during start-up.
  static constexpr int kRoundtripTimeoutMs = 5000;
  /// Maximum DMA-BUF bind version we support (v3: format + modifier events,
  /// create_immed).  Capped regardless of what the compositor advertises.
  static constexpr uint32_t kDmaBufVersion = 3;
  /// DRM format exported from Vulkan.
  /// VK_FORMAT_B8G8R8A8_UNORM ↔ DRM_FORMAT_ARGB8888 (little-endian).
  static constexpr uint32_t kDrmFormat = DRM_FORMAT_ARGB8888;

  // ── Internal pipeline steps ─────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitVulkan();
  bool CreateDmaBufBuffer();
  /// Run the render loop.  Returns true on a clean exit (user closed the
  /// window or pressed ESC), false if the compositor disconnected unexpectedly.
  bool MainLoop();

  /// Register a wl_surface.frame callback with the compositor.
  void RequestFrameCallback() noexcept;

  /// Render one frame: re-record the Vulkan command buffer, submit,
  /// wait for the fence, then arm the callback, attach, damage, and commit.
  void RenderFrame() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler method implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(const uint32_t time_ms) {
  app_->OnFrameReady(time_ms);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  // Send versioned seat/keyboard release requests before member destructors
  // run.  SeatManager::Release() calls wl_keyboard.release (v≥3) then
  // wl_seat.release (v≥5) before the WlPtr destructors fire.
  seat_.Release();

  // Everything else is handled by member destructors in declaration-reverse
  // order:
  //   frame_callback_ → seat_ → xdg_toplevel_ → xdg_surface_ → xdg_wm_base_
  //   → wl_buffer_ (wl_buffer.destroy) → linux_dmabuf_ (destroy request)
  //   → vk_ (VulkanState dtor: close(dma_fd) + Vulkan teardown)
  //   → surface_ (wl_surface.destroy) → compositor_ (wl_proxy_destroy)
  //   → registry_ → display_ (disconnect).
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
  if (!CreateDmaBufBuffer())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "xdg-simple-dmabuf-vulkan: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using wl_comp = wayland::client::wl_compositor_traits;
    using xdg_base = xdg_shell::client::xdg_wm_base_traits;
    using wl_s = wayland::client::wl_seat_traits;
    using dmabuf = linux_dmabuf_unstable_v1::client::zwp_linux_dmabuf_v1_traits;

    if (iface == wl_comp::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == xdg_base::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_s::interface_name) {
      seat_.Record(name, ver);
    } else if (iface == dmabuf::interface_name) {
      linux_dmabuf_name_ = name;
      linux_dmabuf_ver_ = ver;
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: timed out waiting for global advertisements\n");
    return false;
  }

  if (!compositor_name_) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: wl_compositor not advertised\n");
    return false;
  }
  if (!xdg_wm_base_name_) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: xdg_wm_base not advertised\n");
    return false;
  }
  if (!linux_dmabuf_name_) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: zwp_linux_dmabuf_v1 not advertised\n");
    return false;
  }
  return true;
}

// ── BindGlobals ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace linux_dmabuf_unstable_v1::client;

  // wl_compositor — no events; use Attach() to skip listener installation.
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: wl_compositor bind failed\n");
    return false;
  }

  // xdg_wm_base — CRTP handler responds to ping automatically.
  if (!wl::BindHandler<xdg_wm_base_traits>(registry_, xdg_wm_base_,
                                            xdg_wm_base_name_,
                                            xdg_wm_base_ver_)) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: xdg_wm_base bind failed\n");
    return false;
  }

  // zwp_linux_dmabuf_v1 — cap at kDmaBufVersion (v3) regardless of what the
  // compositor advertises; our wl_interface tables only cover v1–v3 messages.
  const uint32_t dmabuf_bind_ver =
      std::min({linux_dmabuf_ver_,
                zwp_linux_dmabuf_v1_traits::version, kDmaBufVersion});
  if (!wl::BindHandler<zwp_linux_dmabuf_v1_traits>(
          registry_, linux_dmabuf_, linux_dmabuf_name_, dmabuf_bind_ver)) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: zwp_linux_dmabuf_v1 bind failed\n");
    return false;
  }

  // Roundtrip so the compositor sends format/modifier events from the
  // linux-dmabuf global before we check format support.
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: timed out waiting for linux-dmabuf formats\n");
    return false;
  }

  if (!linux_dmabuf_.Get()->has_argb8888) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: compositor does not support "
        "DRM_FORMAT_ARGB8888\n");
    return false;
  }

  // wl_seat — optional; SeatManager::Bind() is a no-op if not advertised.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "xdg-simple-dmabuf-vulkan: wl_seat bind failed\n");
    return false;
  }

  // Roundtrip so seat capabilities arrive before CreateSurfaces.
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: timed out waiting for seat capabilities\n");
    return false;
  }
  return true;
}

// ── CreateSurfaces ────────────────────────────────────────────────────────────

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_compositor.create_surface → wl_surface.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: wl_compositor.create_surface failed\n");
    return false;
  }

  // xdg_wm_base.get_xdg_surface → xdg_surface.
  if (!wl::SetupHandler(
          xdg_surface_,
          wl::construct<xdg_surface_traits,
                        xdg_wm_base_traits::Op::GetXdgSurface>(
              *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // xdg_surface.get_toplevel → xdg_toplevel.
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;

  xdg_toplevel_.Get()->SetTitle("xdg-simple-dmabuf-vulkan");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.xdg-simple-dmabuf-vulkan");

  // Empty commit to trigger the initial xdg_surface.configure roundtrip.
  // OnToplevelConfigure will record the compositor-suggested window size;
  // InitVulkan() will then allocate the image at that size.
  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: timed out waiting for xdg_surface configure\n");
    return false;
  }
  return true;
}

// ── InitVulkan ────────────────────────────────────────────────────────────────

bool App::InitVulkan() {
  // ── Instance ───────────────────────────────────────────────────────────────
  // Require Vulkan 1.1 so that VkExternalMemoryImageCreateInfo and
  // VkExportMemoryAllocateInfo are core (no KHR suffix needed).
  const vk::ApplicationInfo app_info{"xdg-simple-dmabuf-vulkan",
                                     VK_MAKE_VERSION(0, 1, 0), nullptr, 0,
                                     VK_API_VERSION_1_1};
  const vk::InstanceCreateInfo inst_ci{{}, &app_info};
  auto inst_rv = vk::createInstanceUnique(inst_ci);
  if (!VkOk(inst_rv.result, "vkCreateInstance"))
    return false;
  vk_.instance = std::move(inst_rv.value);

  // ── Physical device ────────────────────────────────────────────────────────
  auto phys_rv = vk_.instance->enumeratePhysicalDevices();
  if (!VkOk(phys_rv.result, "vkEnumeratePhysicalDevices") ||
      phys_rv.value.empty()) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: no Vulkan physical devices\n");
    return false;
  }
  vk_.phys_dev = phys_rv.value[0];

  // ── Graphics queue family ──────────────────────────────────────────────────
  const auto qfps = vk_.phys_dev.getQueueFamilyProperties();
  for (uint32_t i = 0; i < static_cast<uint32_t>(qfps.size()); ++i) {
    if (qfps[i].queueFlags & vk::QueueFlagBits::eGraphics) {
      vk_.queue_family = i;
      break;
    }
  }
  if (vk_.queue_family == UINT32_MAX) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: no graphics queue family\n");
    return false;
  }

  // ── Device extension check ─────────────────────────────────────────────────
  auto ext_rv = vk_.phys_dev.enumerateDeviceExtensionProperties();
  if (!VkOk(ext_rv.result, "vkEnumerateDeviceExtensionProperties"))
    return false;
  constexpr std::string_view kExtFd = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
  const bool has_ext_fd =
      std::any_of(ext_rv.value.begin(), ext_rv.value.end(),
                  [](const vk::ExtensionProperties& e) {
                    return std::string_view{e.extensionName.data()} == kExtFd;
                  });
  if (!has_ext_fd) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: device missing extension %.*s\n",
                 static_cast<int>(kExtFd.size()), kExtFd.data());
    return false;
  }

  // ── Logical device ─────────────────────────────────────────────────────────
  const float queue_prio = 1.0f;
  const vk::DeviceQueueCreateInfo queue_ci{{}, vk_.queue_family, 1,
                                           &queue_prio};
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
  static constexpr const char* kDevExts[] = {
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
  };
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
  const vk::DeviceCreateInfo dev_ci{
      {}, 1,          &queue_ci, 0, nullptr,
      1,  kDevExts};
  auto dev_rv = vk_.phys_dev.createDeviceUnique(dev_ci);
  if (!VkOk(dev_rv.result, "vkCreateDevice"))
    return false;
  vk_.device = std::move(dev_rv.value);
  vk_.queue = vk_.device->getQueue(vk_.queue_family, 0);

  // ── Command pool ───────────────────────────────────────────────────────────
  const vk::CommandPoolCreateInfo pool_ci{
      vk::CommandPoolCreateFlagBits::eResetCommandBuffer, vk_.queue_family};
  auto pool_rv = vk_.device->createCommandPoolUnique(pool_ci);
  if (!VkOk(pool_rv.result, "vkCreateCommandPool"))
    return false;
  vk_.cmd_pool = std::move(pool_rv.value);

  // ── Exportable image ───────────────────────────────────────────────────────
  // VK_FORMAT_B8G8R8A8_UNORM ↔ DRM_FORMAT_ARGB8888 (little-endian byte order).
  // VK_IMAGE_TILING_LINEAR is required for DMA-BUF export with the LINEAR
  // modifier (DRM_FORMAT_MOD_LINEAR = 0).
  const vk::ExternalMemoryImageCreateInfo ext_img{
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  vk::ImageCreateInfo img_ci;
  img_ci.imageType = vk::ImageType::e2D;
  img_ci.format = vk::Format::eB8G8R8A8Unorm;
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

  auto img_rv = vk_.device->createImageUnique(img_ci);
  if (!VkOk(img_rv.result, "vkCreateImage"))
    return false;
  vk_.image = std::move(img_rv.value);

  // ── Memory allocation ──────────────────────────────────────────────────────
  const vk::MemoryRequirements mem_reqs =
      vk_.device->getImageMemoryRequirements(*vk_.image);

  // Query the row stride — required for the DMA-BUF import by the compositor.
  const vk::SubresourceLayout sub_layout =
      vk_.device->getImageSubresourceLayout(
          *vk_.image, {vk::ImageAspectFlagBits::eColor, 0, 0});
  vk_.stride = static_cast<uint32_t>(sub_layout.rowPitch);

  // Find a memory type that is compatible with the image and prefers
  // device-local (fast GPU-side) memory.
  const vk::PhysicalDeviceMemoryProperties mem_props =
      vk_.phys_dev.getMemoryProperties();
  uint32_t mem_type = UINT32_MAX;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if (!(mem_reqs.memoryTypeBits & (1u << i)))
      continue;
    if (mem_props.memoryTypes[i].propertyFlags &
        vk::MemoryPropertyFlagBits::eDeviceLocal) {
      mem_type = i;
      break;
    }
    if (mem_type == UINT32_MAX)
      mem_type = i;  // accept any compatible type as fallback
  }
  if (mem_type == UINT32_MAX) {
    std::fprintf(stderr,
                 "xdg-simple-dmabuf-vulkan: no suitable Vulkan memory type\n");
    return false;
  }

  const vk::ExportMemoryAllocateInfo export_mem{
      vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  vk::MemoryAllocateInfo mem_ai{mem_reqs.size, mem_type};
  mem_ai.setPNext(&export_mem);

  auto mem_rv = vk_.device->allocateMemoryUnique(mem_ai);
  if (!VkOk(mem_rv.result, "vkAllocateMemory"))
    return false;
  vk_.memory = std::move(mem_rv.value);

  if (!VkOk(vk_.device->bindImageMemory(*vk_.image, *vk_.memory, 0),
            "vkBindImageMemory"))
    return false;

  // ── Export DMA-BUF fd ──────────────────────────────────────────────────────
  // getMemoryFdKHR returns a fresh O_RDWR file descriptor.  We keep it for
  // the lifetime of the application; the compositor receives a dup via the
  // Wayland fd-passing mechanism when we call zwp_linux_buffer_params_v1::add.
  //
  // vkGetMemoryFdKHR is a device-level extension entry point; it is NOT in
  // the static dispatch table.  Load function pointers for this device via a
  // local DispatchLoaderDynamic so vulkan.hpp resolves the symbol at runtime
  // through vkGetDeviceProcAddr rather than requiring a link-time symbol.
  const vk::DispatchLoaderDynamic dld(*vk_.instance, vkGetInstanceProcAddr,
                                      *vk_.device);
  const vk::MemoryGetFdInfoKHR fd_info{
      *vk_.memory, vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT};
  auto fd_rv = vk_.device->getMemoryFdKHR(fd_info, dld);
  if (!VkOk(fd_rv.result, "vkGetMemoryFdKHR"))
    return false;
  vk_.dma_fd = fd_rv.value;

  // ── Command buffer ─────────────────────────────────────────────────────────
  const vk::CommandBufferAllocateInfo cb_ai{
      *vk_.cmd_pool, vk::CommandBufferLevel::ePrimary, 1};
  auto cb_rv = vk_.device->allocateCommandBuffersUnique(cb_ai);
  if (!VkOk(cb_rv.result, "vkAllocateCommandBuffers"))
    return false;
  vk_.cmd_buf = std::move(cb_rv.value.front());

  // ── Fence ──────────────────────────────────────────────────────────────────
  auto fence_rv = vk_.device->createFenceUnique({});
  if (!VkOk(fence_rv.result, "vkCreateFence"))
    return false;
  vk_.fence = std::move(fence_rv.value);

  vulkan_init_ = true;
  std::printf("xdg-simple-dmabuf-vulkan: Vulkan initialised "
              "(stride=%u, dma_fd=%d)\n",
              vk_.stride, vk_.dma_fd);
  return true;
}

// ── CreateDmaBufBuffer ────────────────────────────────────────────────────────

bool App::CreateDmaBufBuffer() {
  using namespace linux_dmabuf_unstable_v1::client;
  using namespace wayland::client;

  // Create a transient zwp_linux_buffer_params_v1 to build the wl_buffer.
  // We use Attach() (not SetupHandler) because we take the create_immed
  // synchronous path and never need to receive created/failed events.
  wl::WlPtr<LinuxBufferParamsHandler> params;
  if (wl_proxy* raw =
          wl::construct<zwp_linux_buffer_params_v1_traits,
                        zwp_linux_dmabuf_v1_traits::Op::CreateParams>(
              *linux_dmabuf_.Get())) {
    params.Attach(raw);
  } else {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: zwp_linux_dmabuf_v1.create_params failed\n");
    return false;
  }

  // Add our DMA-BUF plane.  The fd is sent via SCM_RIGHTS (libwayland dups it
  // during marshalling); our copy vk_.dma_fd remains valid afterwards.
  // Modifier: DRM_FORMAT_MOD_LINEAR = 0.
  params.Get()->Add(
      vk_.dma_fd, 0u, 0u, vk_.stride,
      static_cast<uint32_t>(static_cast<uint64_t>(DRM_FORMAT_MOD_LINEAR) >>
                            32u),
      static_cast<uint32_t>(DRM_FORMAT_MOD_LINEAR));

  // create_immed synchronously returns a wl_buffer proxy on the client side
  // without waiting for a compositor round-trip.
  if (wl_proxy* raw = wl::construct<wl_buffer_traits,
                                    zwp_linux_buffer_params_v1_traits::Op::
                                        CreateImmed>(
          *params.Get(), static_cast<int32_t>(width_),
          static_cast<int32_t>(height_), kDrmFormat, 0u)) {
    // Use _SetProxy to install the event listener for wl_buffer.release.
    wl_buffer_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(
        stderr,
        "xdg-simple-dmabuf-vulkan: zwp_linux_buffer_params_v1.create_immed "
        "failed\n");
    return false;
  }

  // params goes out of scope here — WlPtr::~WlPtr() sends
  // zwp_linux_buffer_params_v1.destroy and calls wl_proxy_destroy.
  return true;
}

// ── MainLoop ──────────────────────────────────────────────────────────────────

bool App::MainLoop() {
  std::printf(
      "xdg-simple-dmabuf-vulkan: entering render loop (ESC or close to "
      "quit)\n");

  // Kickstart: render frame 0 (Vulkan GPU work), arm the first frame callback,
  // attach the DMA-BUF-backed wl_buffer, and commit.
  RenderFrame();

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; },
      "xdg-simple-dmabuf-vulkan");
  if (ok)
    std::printf("xdg-simple-dmabuf-vulkan: exiting cleanly\n");
  return ok;
}

// ── RequestFrameCallback ──────────────────────────────────────────────────────

void App::RequestFrameCallback() noexcept {
  // wl_surface.frame → wl_callback.  Must be called BEFORE wl_surface.commit
  // so that the callback registration and the buffer attachment land in the
  // same compositor message batch.
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_callback_.Get()->app_ = this;
    frame_callback_.Get()->_SetProxy(raw);
  }
}

// ── RenderFrame ───────────────────────────────────────────────────────────────

void App::RenderFrame() noexcept {
  // ── Colour cycle (hue shift matching simple-egl) ──────────────────────────
  const auto t = static_cast<float>(frame_ % 256u) / 255.0f;
  const std::array<float, 4> colour{t, 0.3f, 0.5f, 1.0f};

  // ── Re-record the command buffer ──────────────────────────────────────────
  if (vk_.cmd_buf->reset({}) != vk::Result::eSuccess) {
    running_ = false;
    return;
  }
  if (vk_.cmd_buf->begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) !=
      vk::Result::eSuccess) {
    running_ = false;
    return;
  }

  // Subresource range covering the whole image (single colour mip+layer).
  const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1,
                                        0, 1};

  // Transition: UNDEFINED (first frame) or GENERAL → TRANSFER_DST_OPTIMAL.
  // vkCmdClearColorImage requires TRANSFER_DST_OPTIMAL or GENERAL; we move
  // to TRANSFER_DST_OPTIMAL for the clear and then back to GENERAL so the
  // compositor can safely read the DMA-BUF pixels via DMA.
  const vk::ImageLayout old_layout = (frame_ == 0)
                                         ? vk::ImageLayout::eUndefined
                                         : vk::ImageLayout::eGeneral;
  const vk::ImageMemoryBarrier to_clear{
      vk::AccessFlagBits::eNone,
      vk::AccessFlagBits::eTransferWrite,
      old_layout,
      vk::ImageLayout::eTransferDstOptimal,
      VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED,
      *vk_.image,
      range};
  vk_.cmd_buf->pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                vk::PipelineStageFlagBits::eTransfer, {}, {},
                                {}, to_clear);

  vk_.cmd_buf->clearColorImage(*vk_.image,
                                vk::ImageLayout::eTransferDstOptimal,
                                vk::ClearColorValue{colour}, range);

  // Transition: TRANSFER_DST_OPTIMAL → GENERAL.
  // eMemoryRead covers both host reads (flush/invalidate) and DMA reads from
  // the compositor.  eBottomOfPipe as destination stage ensures the barrier
  // is not optimised away before the queue submission fence signals.
  const vk::ImageMemoryBarrier to_general{
      vk::AccessFlagBits::eTransferWrite,
      vk::AccessFlagBits::eMemoryRead,
      vk::ImageLayout::eTransferDstOptimal,
      vk::ImageLayout::eGeneral,
      VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED,
      *vk_.image,
      range};
  vk_.cmd_buf->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eBottomOfPipe, {},
                                {}, {}, to_general);

  if (vk_.cmd_buf->end() != vk::Result::eSuccess) {
    running_ = false;
    return;
  }

  // ── Submit and wait for GPU completion ────────────────────────────────────
  // Waiting on the fence before attaching the buffer ensures the compositor
  // reads fully rendered pixels.  Implicit DMA-BUF fence support in the
  // kernel would remove the need for this CPU stall; we use the simple
  // synchronous path here to keep the example self-contained.
  const vk::SubmitInfo submit_info{{}, {}, *vk_.cmd_buf, {}};
  if (vk_.queue.submit(1, &submit_info, *vk_.fence) != vk::Result::eSuccess) {
    running_ = false;
    return;
  }
  if (vk_.device->waitForFences(*vk_.fence, VK_TRUE, UINT64_MAX) !=
      vk::Result::eSuccess) {
    running_ = false;
    return;
  }
  if (!VkOk(vk_.device->resetFences(*vk_.fence), "vkResetFences")) {
    running_ = false;
    return;
  }

  // ── Present via Wayland ───────────────────────────────────────────────────
  // Mark the buffer as "in use" so OnRelease() can track compositor ownership.
  wl_buffer_.Get()->released_ = false;
  // Register the frame callback BEFORE commit so the request lands in the
  // same message batch as the buffer attachment (required by the Wayland
  // protocol — the callback fires after the frame that contains it).
  RequestFrameCallback();
  surface_.Get()->Attach(wl_buffer_.Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, width_, height_);
  surface_.Get()->Commit();

  ++frame_;
}

// ── OnFrameReady ──────────────────────────────────────────────────────────────

void App::OnFrameReady(uint32_t /*time_ms*/) noexcept {
  // Detach the now-spent wl_callback proxy before arming the next one.
  // Detach() sets m_proxy to nullptr so WlPtr::~WlPtr() will not double-free.
  wl_proxy* const spent_cb = frame_callback_.Detach();
  const auto guard = wl::ScopeExit{[spent_cb] {
    if (spent_cb)
      wl_proxy_destroy(spent_cb);
  }};

  // RenderFrame() re-arms the callback, submits GPU work, and commits.
  RenderFrame();
}

// ── App callbacks ─────────────────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
  // Accept a compositor-suggested size only before Vulkan is initialised.
  // After that the DMA-BUF image size is fixed; dynamic resize would require
  // reallocating the image and recreating the wl_buffer, which is beyond the
  // scope of this introductory example.
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0 && !vulkan_init_) {
    width_ = std::min(w, kMaxDim);
    height_ = std::min(h, kMaxDim);
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnKey(const uint32_t key, const uint32_t state) {
  if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  // Suppress SIGPIPE so a compositor disconnect during wl_display_flush is
  // reported as EPIPE / error return rather than terminating the process.
  std::signal(SIGPIPE, SIG_IGN);

  App app;
  return app.Run();
}
