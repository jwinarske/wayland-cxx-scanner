// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// skia-egl-canvas — renders the shared demo scene with Skia's Ganesh GL backend
// through an EGL / wayland-egl surface.
//
// A GrDirectContext drives OpenGL ES via the default framebuffer (FBO 0), which
// is wrapped in an SkSurface each frame and drawn with the same DemoScene the
// wl_shm example uses, so the two backends are directly comparable.
//
// Controls:
//   ESC / window close   quit
//   SPACE                toggles the button-active scene state

// ── EGL/GLES headers (before Wayland headers)
// ─────────────────────────────────
extern "C" {
#include <EGL/egl.h>
#include <GLES2/gl2.h>
}

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── System Wayland C headers
// ──────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
}

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── Shared scene
// ──────────────────────────────────────────────────────────────
#include "scene.hpp"

// ── Skia (Ganesh GL)
// ──────────────────────────────────────────────────────────
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkColorType.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"
#include "include/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h"

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

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

}  // namespace wayland::client

namespace {

// GL sized internal format of the default framebuffer (GL_RGBA8); the EGL
// config requests 8-bit RGBA.
constexpr unsigned int kGlRgba8 = 0x8058u;

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

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  void OnXdgSurfaceConfigure(std::uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose() { running_ = false; }
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameReady(std::uint32_t time_ms) noexcept;

 private:
  static constexpr int kDefaultWidth = 480;
  static constexpr int kDefaultHeight = 320;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitEgl();
  bool MainLoop();
  void RequestFrameCallback() noexcept;
  void RenderFrame() noexcept;

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlSurfaceHandler> surface_;

  // EGL state — torn down (as a member) before surface_/compositor_.
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
      if (window != nullptr)
        wl_egl_window_destroy(window);
      eglTerminate(display);
    }
    EglState() = default;
    EglState(const EglState&) = delete;
    EglState& operator=(const EglState&) = delete;
    EglState(EglState&&) = delete;
    EglState& operator=(EglState&&) = delete;
  } egl_;

  // Ganesh context — declared after egl_ so it is destroyed first, while the
  // GL context is still current.
  sk_sp<GrDirectContext> gr_context_;

  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;
  wl::SeatManager<App> seat_;
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  bool running_ = true;
  bool configured_ = false;
  int width_ = kDefaultWidth;
  int height_ = kDefaultHeight;

  std::uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  std::uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  demo::SceneState scene_;
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
  // Release GL-backed resources while the EGL context is still current.
  gr_context_.reset();
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
  if (!InitEgl())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "skia-egl-canvas: wl_display_connect failed\n");
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "skia-egl-canvas: registry creation failed\n");
    return false;
  }
  registry_.OnGlobal([this](wl::CRegistry&, std::uint32_t name,
                            std::string_view iface, std::uint32_t ver) {
    using namespace wayland::client;
    using namespace xdg_shell::client;
    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    }
  });
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "skia-egl-canvas: timed out waiting for globals\n");
    return false;
  }
  if (!compositor_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "skia-egl-canvas: required globals not found\n");
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
    std::fprintf(stderr, "skia-egl-canvas: wl_compositor bind failed\n");
    return false;
  }
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "skia-egl-canvas: xdg_wm_base bind failed\n");
    return false;
  }
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "skia-egl-canvas: wl_seat bind failed\n");
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
    std::fprintf(stderr, "skia-egl-canvas: create_surface failed\n");
    return false;
  }

  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "skia-egl-canvas: get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "skia-egl-canvas: get_toplevel failed\n");
    return false;
  }
  auto* toplevel = xdg_toplevel_.Get();
  toplevel->app_ = this;
  toplevel->SetTitle("skia-egl-canvas");
  toplevel->SetAppId("org.wayland-cxx.skia-egl-canvas");

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "skia-egl-canvas: timed out waiting for configure\n");
    return false;
  }
  return true;
}

bool App::InitEgl() {
  egl_.display = eglGetDisplay(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeDisplayType>(display_.Get()));
  if (egl_.display == EGL_NO_DISPLAY ||
      !eglInitialize(egl_.display, nullptr, nullptr)) {
    std::fprintf(stderr, "skia-egl-canvas: eglInitialize failed\n");
    return false;
  }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    std::fprintf(stderr, "skia-egl-canvas: eglBindAPI failed\n");
    return false;
  }

  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
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
      EGL_STENCIL_SIZE,
      8,
      EGL_NONE,
  };
  static constexpr EGLint kCtxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)

  EGLint num_configs = 0;
  if (!eglChooseConfig(egl_.display, std::data(kConfigAttribs), &egl_.config, 1,
                       &num_configs) ||
      num_configs < 1) {
    std::fprintf(stderr, "skia-egl-canvas: eglChooseConfig failed\n");
    return false;
  }

  egl_.context = eglCreateContext(egl_.display, egl_.config, EGL_NO_CONTEXT,
                                  std::data(kCtxAttribs));
  if (egl_.context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "skia-egl-canvas: eglCreateContext failed\n");
    return false;
  }

  egl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()), width_,
      height_);
  if (egl_.window == nullptr) {
    std::fprintf(stderr, "skia-egl-canvas: wl_egl_window_create failed\n");
    return false;
  }

  egl_.surface = eglCreateWindowSurface(
      egl_.display, egl_.config,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeWindowType>(egl_.window), nullptr);
  if (egl_.surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "skia-egl-canvas: eglCreateWindowSurface failed\n");
    return false;
  }

  if (!eglMakeCurrent(egl_.display, egl_.surface, egl_.surface, egl_.context)) {
    std::fprintf(stderr, "skia-egl-canvas: eglMakeCurrent failed\n");
    return false;
  }
  eglSwapInterval(egl_.display, 1);

  gr_context_ = GrDirectContexts::MakeGL(GrGLInterfaces::MakeEGL());
  if (gr_context_ == nullptr) {
    std::fprintf(stderr, "skia-egl-canvas: GrDirectContexts::MakeGL failed\n");
    return false;
  }
  return true;
}

void App::RenderFrame() noexcept {
  GrGLFramebufferInfo fb_info;
  fb_info.fFBOID = 0;
  fb_info.fFormat = kGlRgba8;
  const GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(
      width_, height_, /*sampleCnt=*/0, /*stencilBits=*/8, fb_info);

  sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(
      gr_context_.get(), target, kBottomLeft_GrSurfaceOrigin,
      kRGBA_8888_SkColorType, nullptr, nullptr);
  if (surface != nullptr) {
    scene_.width = width_;
    scene_.height = height_;
    demo::DemoScene::Render(surface->getCanvas(), scene_, nullptr);
    gr_context_->flushAndSubmit();
  }

  eglSwapBuffers(egl_.display, egl_.surface);
  ++scene_.frame;
}

bool App::MainLoop() {
  std::printf("skia-egl-canvas: press ESC or Ctrl-C to quit\n");
  RequestFrameCallback();
  RenderFrame();
  return wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; },
      "skia-egl-canvas");
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
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    width_ = std::min(w, kMaxDim);
    height_ = std::min(h, kMaxDim);
    if (egl_.window != nullptr)
      wl_egl_window_resize(egl_.window, width_, height_, 0, 0);
  }
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;
  if (ev.key == KEY_ESC)
    running_ = false;
  else if (ev.key == KEY_SPACE)
    scene_.button_active = !scene_.button_active;
}

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, OnSigint);
  App app;
  return app.Run();
}
