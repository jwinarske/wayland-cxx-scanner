// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// shadertoy-vulkan — run any Shadertoy "Image" shader on Wayland via Vulkan.
//
// Connects to the compositor, creates an XDG toplevel, and renders a Shadertoy
// shader (a file argument, or a built-in default) into a Vulkan swapchain.  The
// GLSL is compiled to SPIR-V at runtime, so any single-pass Image shader loads
// without an offline step.  All Vulkan rendering is provided by the platform-
// agnostic shadertoy-cxx library (shadertoy::VkRenderer); this file only
// supplies the Wayland window, input, and the VkSurfaceKHR.
//
// Usage:  shadertoy_vulkan [shader.frag]   (mouse/touch drag → iMouse)
//
// Build requirements: wayland-client, vulkan (VK_KHR_wayland_surface),
//                     wayland-protocols, xkbcommon, shadertoy-cxx.
// Runtime requirement: a Wayland compositor with xdg-shell support.

// Enable the Wayland WSI entry points (vkCreateWaylandSurfaceKHR) before any
// Vulkan header is pulled in (shadertoy/vk_renderer.hpp includes vulkan.h).
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── System Wayland C headers ─────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>  // KEY_ESC, BTN_LEFT
#include <poll.h>
#include <wayland-client-protocol.h>  // wl_*_interface symbols
}

// ── Framework headers ────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/cursor.hpp>
#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── shadertoy-cxx (platform-agnostic renderer) ───────────────────────────────
#include <shadertoy/inputs.hpp>
#include <shadertoy/vk_renderer.hpp>

// ── Standard library ─────────────────────────────────────────────────────────
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

namespace wayland::client {
const wl_interface& wl_compositor_traits::wl_iface() noexcept {
  return wl_compositor_interface;
}
const wl_interface& wl_shm_traits::wl_iface() noexcept {
  return wl_shm_interface;
}
const wl_interface& wl_surface_traits::wl_iface() noexcept {
  return wl_surface_interface;
}
}  // namespace wayland::client

class App;

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};
// Bound only so libwayland-cursor can allocate the cursor theme's buffers.
class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {};
class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// Pointer and touch (both feeding iMouse) are handled by wl::SeatManager<App>:
// it binds the pointer/touch on the seat capabilities event because App defines
// the OnPointer*/OnTouch* hooks below, and forwards typed wl::PointerEvent /
// wl::TouchPoint values to them.

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════
class App {
 public:
  explicit App(std::string shader_path)
      : shader_path_(std::move(shader_path)) {}
  ~App();

  int Run();

  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose();
  void OnKey(const wl::KeyEvent& ev);

  // ── Pointer / touch hooks (dispatched by wl::SeatManager) ─────────────────
  void OnPointerEnter(const wl::PointerEvent& ev) noexcept;
  void OnPointerMotion(const wl::PointerEvent& ev) noexcept;
  void OnPointerButton(const wl::PointerButtonEvent& ev) noexcept;
  void OnTouchDown(const wl::TouchPoint& p) noexcept;
  void OnTouchMotion(const wl::TouchPoint& p) noexcept;
  void OnTouchUp(int32_t id) noexcept;

 private:
  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  wl::SeatManager<App> seat_;    // keyboard + pointer + touch (iMouse)
  wl::WlPtr<WlShmHandler> shm_;  // bound only for the cursor theme
  wl::CursorManager cursor_;     // set_cursor for the seat's pointer
  uint32_t enter_serial_ = 0;    // last wl_pointer.enter serial, for set_cursor

  // Renderer lives in shadertoy-cxx; created once the surface exists.
  std::unique_ptr<shadertoy::VkRenderer> renderer_;
  shadertoy::ShaderInputs inputs_;
  std::string shader_path_;

  bool running_ = true;
  bool configured_ = false;
  uint32_t width_ = 800;
  uint32_t height_ = 600;

  float mouse_cur_x_ = 0.0f, mouse_cur_y_ = 0.0f;
  float mouse_click_x_ = 0.0f, mouse_click_y_ = 0.0f;
  bool mouse_down_ = false;
  int32_t touch_id_ = -1;  // active touch point id, -1 = none

  using Clock = std::chrono::steady_clock;
  Clock::time_point start_{};
  Clock::time_point last_frame_{};

  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitRenderer();
  bool MainLoop();

  bool PumpWayland() noexcept;
  void UpdateInputs() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
App::~App() {
  // Destroy the renderer (and its VkSurfaceKHR) before the wl_surface proxy.
  renderer_.reset();
  // SeatManager sends the versioned keyboard/pointer/touch releases before its
  // WlPtr members are destroyed.
  seat_.Release();
}

int App::Run() {
  if (!ConnectDisplay() || !ScanGlobals() || !BindGlobals() ||
      !CreateSurfaces() || !InitRenderer())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "shadertoy-vulkan: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "shadertoy-vulkan: get_registry failed\n");
    return false;
  }
  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using wl_comp = wayland::client::wl_compositor_traits;
    using wl_shm = wayland::client::wl_shm_traits;
    using xdg_base = xdg_shell::client::xdg_wm_base_traits;
    using wl_s = wayland::client::wl_seat_traits;
    if (iface == wl_comp::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_shm::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == xdg_base::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_s::interface_name) {
      seat_.Record(name, ver);
    }
  });
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "shadertoy-vulkan: timed out waiting for globals\n");
    return false;
  }
  if (!compositor_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr,
                 "shadertoy-vulkan: missing wl_compositor or xdg_wm_base\n");
    return false;
  }
  return true;
}

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "shadertoy-vulkan: wl_compositor bind failed\n");
    return false;
  }
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "shadertoy-vulkan: xdg_wm_base bind failed\n");
    return false;
  }
  // SeatManager binds keyboard and — since App defines OnPointer*/OnTouch*
  // hooks — the pointer and touch, on the seat capabilities event.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "shadertoy-vulkan: wl_seat bind failed\n");
    return false;
  }
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "shadertoy-vulkan: timed out waiting for seat caps\n");
    return false;
  }

  // wl_shm + cursor theme are optional (the demo renders via Vulkan): bind
  // wl_shm only to give libwayland-cursor a place for the cursor buffers.
  if (shm_name_ &&
      wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    (void)cursor_.Init(shm_.Get()->GetProxy(), compositor_.Get()->GetProxy());
  }
  return true;
}

void App::OnTouchDown(const wl::TouchPoint& p) noexcept {
  if (touch_id_ != -1)
    return;  // track only the first active point
  touch_id_ = p.id;
  mouse_cur_x_ = static_cast<float>(p.x);
  mouse_cur_y_ = static_cast<float>(height_) - static_cast<float>(p.y);
  mouse_click_x_ = mouse_cur_x_;
  mouse_click_y_ = mouse_cur_y_;
  mouse_down_ = true;
}

void App::OnTouchMotion(const wl::TouchPoint& p) noexcept {
  if (p.id != touch_id_)
    return;
  mouse_cur_x_ = static_cast<float>(p.x);
  mouse_cur_y_ = static_cast<float>(height_) - static_cast<float>(p.y);
}

void App::OnTouchUp(int32_t id) noexcept {
  if (id != touch_id_)
    return;
  touch_id_ = -1;
  mouse_down_ = false;
}

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "shadertoy-vulkan: create_surface failed\n");
    return false;
  }
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "shadertoy-vulkan: get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "shadertoy-vulkan: get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;
  xdg_toplevel_.Get()->SetTitle("shadertoy-vulkan");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.shadertoy-vulkan");

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "shadertoy-vulkan: timed out waiting for configure\n");
    return false;
  }
  return true;
}

bool App::InitRenderer() {
  std::string src;
  if (!shader_path_.empty()) {
    src = shadertoy::LoadShaderFile(shader_path_);
    if (src.empty())
      std::fprintf(stderr, "shadertoy-vulkan: cannot read %s; using default\n",
                   shader_path_.c_str());
  }
  if (src.empty())
    src = shadertoy::DefaultImageShader();

  shadertoy::VkRendererConfig cfg;
  cfg.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                             VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
  cfg.width = width_;
  cfg.height = height_;
  cfg.image_shader = src;

  wl_display* dpy = display_.Get();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* wl_surf = reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy());
  cfg.create_surface = [dpy, wl_surf](VkInstance instance) -> VkSurfaceKHR {
    VkWaylandSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = dpy;
    info.surface = wl_surf;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &surface) !=
        VK_SUCCESS)
      return VK_NULL_HANDLE;
    return surface;
  };

  renderer_ = shadertoy::VkRenderer::Create(cfg);
  if (!renderer_) {
    std::fprintf(stderr, "shadertoy-vulkan: renderer creation failed\n");
    return false;
  }
  start_ = Clock::now();
  last_frame_ = start_;
  return true;
}

// Non-blocking Wayland event pump (the Vulkan swapchain paces presentation, so
// we never block on the display fd).
bool App::PumpWayland() noexcept {
  wl_display* d = display_.Get();
  while (wl_display_prepare_read(d) != 0) {
    if (wl_display_dispatch_pending(d) < 0)
      return false;
  }
  if (wl_display_flush(d) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(d);
    return false;
  }
  pollfd pfd{wl_display_get_fd(d), POLLIN, 0};
  const int n = poll(&pfd, 1, 0);
  if (n > 0 && (pfd.revents & POLLIN) != 0) {
    if (wl_display_read_events(d) < 0)
      return false;
  } else {
    wl_display_cancel_read(d);
  }
  return wl_display_dispatch_pending(d) >= 0;
}

bool App::MainLoop() {
  std::printf("shadertoy-vulkan: rendering (ESC or close to quit)\n");
  while (running_) {
    if (!PumpWayland()) {
      wl::LogWlError(display_.Get(), "event pump", "shadertoy-vulkan");
      return false;
    }
    UpdateInputs();
    if (!renderer_->Render(inputs_)) {
      std::fprintf(stderr, "shadertoy-vulkan: render failed\n");
      return false;
    }
    ++inputs_.frame;
  }
  return true;
}

void App::UpdateInputs() noexcept {
  const auto now = Clock::now();
  const float t = std::chrono::duration<float>(now - start_).count();
  const float dt = std::chrono::duration<float>(now - last_frame_).count();
  last_frame_ = now;

  inputs_.res_x = static_cast<float>(width_);
  inputs_.res_y = static_cast<float>(height_);
  inputs_.res_z = 1.0f;
  inputs_.time = t;
  inputs_.time_delta = dt;
  inputs_.frame_rate = dt > 0.0f ? 1.0f / dt : 60.0f;
  inputs_.mouse_x = mouse_cur_x_;
  inputs_.mouse_y = mouse_cur_y_;
  inputs_.mouse_z = mouse_down_ ? mouse_click_x_ : -mouse_click_x_;
  inputs_.mouse_w = mouse_down_ ? mouse_click_y_ : -mouse_click_y_;

  const std::time_t tt = std::time(nullptr);
  std::tm tm_buf{};
  if (localtime_r(&tt, &tm_buf) != nullptr) {
    inputs_.date_y = static_cast<float>(tm_buf.tm_year + 1900);
    inputs_.date_m = static_cast<float>(tm_buf.tm_mon);
    inputs_.date_d = static_cast<float>(tm_buf.tm_mday);
    inputs_.date_s = static_cast<float>(tm_buf.tm_hour * 3600 +
                                        tm_buf.tm_min * 60 + tm_buf.tm_sec);
  }
}

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    width_ = static_cast<uint32_t>(std::min(w, kMaxDim));
    height_ = static_cast<uint32_t>(std::min(h, kMaxDim));
    if (renderer_)
      renderer_->Resize(width_, height_);
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.key == KEY_ESC && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

// The pointer enter carries a position too; treat it like the first motion.
void App::OnPointerEnter(const wl::PointerEvent& ev) noexcept {
  enter_serial_ = ev.serial;
  cursor_.Reset();
  cursor_.Set(seat_.Pointer(), enter_serial_, "default");
  OnPointerMotion(ev);
}

void App::OnPointerMotion(const wl::PointerEvent& ev) noexcept {
  mouse_cur_x_ = static_cast<float>(ev.x);
  mouse_cur_y_ = static_cast<float>(height_) - static_cast<float>(ev.y);
}

void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  if (ev.button != BTN_LEFT)
    return;
  mouse_down_ = (ev.state == WL_POINTER_BUTTON_STATE_PRESSED);
  if (mouse_down_) {
    mouse_click_x_ = mouse_cur_x_;
    mouse_click_y_ = mouse_cur_y_;
  }
}

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::string shader_path = (argc > 1) ? argv[1] : std::string();
  App app(std::move(shader_path));
  return app.Run();
}
