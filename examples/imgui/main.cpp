// SPDX-License-Identifier: MIT
//
// imgui-demo — Dear ImGui demo window on Wayland via wayland-cxx-scanner.
//
// Platform backend: imgui_impl_wayland_cxx (this repository)
// Renderer backend: imgui_impl_opengl3 (upstream, OpenGL ES 2 via EGL)
//
// Structure follows the wayland-cxx-scanner `simple-egl` example: CRTP
// handlers for xdg-shell, a frame-callback-paced render loop, and
// wl::RunEventLoop with the keyboard-repeat fd wired to the ImGui backend.

// ── EGL/GLES headers (before Wayland headers) ────────────────────────────────
extern "C" {
#include <EGL/egl.h>
#include <GLES2/gl2.h>
}

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── System Wayland C headers ─────────────────────────────────────────────────
extern "C" {
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
}

// ── Framework headers ────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── Dear ImGui ───────────────────────────────────────────────────────────────
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_wayland_cxx.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions owned by the application.
//
// The ImGui backend provides wl_seat / wl_keyboard (via <wl/seat.hpp>) and
// wl_pointer / wl_touch / wl_shm.  The application supplies the interfaces it
// binds itself; xdg-shell tables come inline from <wl/xdg_shell.hpp>.
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

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handlers
// ══════════════════════════════════════════════════════════════════════════════

class App;

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // Callbacks from the xdg-shell CRTP handlers (<wl/xdg_shell.hpp>).
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose();
  void OnFrameReady(uint32_t time_ms) noexcept;

 private:
  // Declaration order = reverse destruction order.
  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlSurfaceHandler> surface_;

  struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = {};
    wl_egl_window* window = nullptr;

    ~EglState() noexcept {
      if (display == EGL_NO_DISPLAY)
        return;
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (surface != EGL_NO_SURFACE)
        eglDestroySurface(display, surface);
      if (context != EGL_NO_CONTEXT)
        eglDestroyContext(display, context);
      if (window)
        wl_egl_window_destroy(window);
      eglTerminate(display);
    }
    EglState() = default;
    EglState(const EglState&) = delete;
    EglState& operator=(const EglState&) = delete;
  } egl_;

  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  bool running_ = true;
  bool imgui_up_ = false;
  int width_ = 1024;
  int height_ = 640;

  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitEgl();
  bool InitImGui();
  bool MainLoop();
  void ShutdownImGui();

  void RequestFrameCallback() noexcept;
  void RenderFrame() noexcept;
};

void WlCallbackHandler::OnDone(const uint32_t time_ms) {
  app_->OnFrameReady(time_ms);
}

App::~App() {
  ShutdownImGui();  // releases the backend's seat/pointer/etc. first
  // Remaining teardown via member destructors, reverse declaration order.
}

int App::Run() {
  if (!ConnectDisplay() || !ScanGlobals() || !BindGlobals() ||
      !CreateSurfaces() || !InitEgl() || !InitImGui())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "imgui-demo: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "imgui-demo: wl_display_get_registry failed\n");
    return false;
  }
  registry_.OnGlobal([this](wl::CRegistry&, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using wl_comp = wayland::client::wl_compositor_traits;
    using xdg_base = xdg_shell::client::xdg_wm_base_traits;
    if (iface == wl_comp::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == xdg_base::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    }
    // wl_seat / wl_shm are discovered and bound by the ImGui backend on its
    // own registry — the app does not touch input at all.
  });
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "imgui-demo: registry roundtrip timed out\n");
    return false;
  }
  if (!compositor_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "imgui-demo: wl_compositor/xdg_wm_base missing\n");
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
    std::fprintf(stderr, "imgui-demo: wl_compositor bind failed\n");
    return false;
  }
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "imgui-demo: xdg_wm_base bind failed\n");
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
    std::fprintf(stderr, "imgui-demo: create_surface failed\n");
    return false;
  }

  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "imgui-demo: get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "imgui-demo: get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;
  xdg_toplevel_.Get()->SetTitle("Dear ImGui — wayland-cxx-scanner");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.imgui-demo");

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "imgui-demo: configure roundtrip timed out\n");
    return false;
  }
  return true;
}

bool App::InitEgl() {
  egl_.display = eglGetDisplay(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
      static_cast<EGLNativeDisplayType>(display_.Get()));
  if (egl_.display == EGL_NO_DISPLAY)
    return false;
  if (!eglInitialize(egl_.display, nullptr, nullptr))
    return false;
  if (!eglBindAPI(EGL_OPENGL_ES_API))
    return false;

  static const EGLint kConfigAttribs[] = {
      EGL_SURFACE_TYPE,
      EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      8,
      EGL_NONE,
  };
  EGLint num_configs = 0;
  if (!eglChooseConfig(egl_.display, kConfigAttribs, &egl_.config, 1,
                       &num_configs) ||
      num_configs < 1)
    return false;

  static constexpr EGLint kCtxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  egl_.context =
      eglCreateContext(egl_.display, egl_.config, EGL_NO_CONTEXT, kCtxAttribs);
  if (egl_.context == EGL_NO_CONTEXT)
    return false;

  egl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()), width_,
      height_);
  if (!egl_.window)
    return false;

  egl_.surface = eglCreateWindowSurface(
      egl_.display, egl_.config,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeWindowType>(egl_.window), nullptr);
  if (egl_.surface == EGL_NO_SURFACE)
    return false;
  if (!eglMakeCurrent(egl_.display, egl_.surface, egl_.surface, egl_.context))
    return false;
  eglSwapInterval(egl_.display, 1);
  return true;
}

bool App::InitImGui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  if (!ImGui_ImplWaylandCxx_Init(
          display_.Get(),
          reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()),
          reinterpret_cast<wl_compositor*>(compositor_.Get()->GetProxy()))) {
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    std::fprintf(stderr, "imgui-demo: platform backend init failed\n");
    return false;
  }
  ImGui_ImplWaylandCxx_SetDisplaySize(width_, height_);
  ImGui_ImplWaylandCxx_SetContentScale(1.0f);

  if (!ImGui_ImplOpenGL3_Init("#version 100")) {  // GLSL ES 1.00 (GLES2)
    std::fprintf(stderr, "imgui-demo: renderer backend init failed\n");
    return false;
  }
  imgui_up_ = true;
  return true;
}

void App::ShutdownImGui() {
  if (!imgui_up_)
    return;
  imgui_up_ = false;
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplWaylandCxx_Shutdown();
  ImGui::DestroyContext();
}

bool App::MainLoop() {
  RequestFrameCallback();
  RenderFrame();

  // The 5-argument RunEventLoop polls the keyboard-repeat timerfd alongside
  // the Wayland fd, so held keys keep producing text at the compositor's
  // advertised rate.
  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "imgui-demo",
      [] { return ImGui_ImplWaylandCxx_GetKeyRepeatFd(); },
      [] { ImGui_ImplWaylandCxx_DispatchKeyRepeat(); });
  return ok;
}

void App::RenderFrame() noexcept {
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplWaylandCxx_NewFrame();
  ImGui::NewFrame();

  ImGui::ShowDemoWindow();

  ImGui::Render();
  const ImGuiIO& io = ImGui::GetIO();
  glViewport(
      0, 0,
      static_cast<GLsizei>(io.DisplaySize.x * io.DisplayFramebufferScale.x),
      static_cast<GLsizei>(io.DisplaySize.y * io.DisplayFramebufferScale.y));
  glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  eglSwapBuffers(egl_.display, egl_.surface);
}

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_callback_.Get()->app_ = this;
    frame_callback_.Get()->_SetProxy(raw);
  }
}

void App::OnFrameReady(uint32_t /*time_ms*/) noexcept {
  wl_proxy* const spent_cb = frame_callback_.Detach();
  const auto guard = wl::ScopeExit{[spent_cb] {
    if (spent_cb)
      wl_proxy_destroy(spent_cb);
  }};
  RequestFrameCallback();
  RenderFrame();
}

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    width_ = std::min(w, kMaxDim);
    height_ = std::min(h, kMaxDim);
    if (egl_.window)
      wl_egl_window_resize(egl_.window, width_, height_, 0, 0);
    ImGui_ImplWaylandCxx_SetDisplaySize(width_, height_);
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  App app;
  return app.Run();
}
