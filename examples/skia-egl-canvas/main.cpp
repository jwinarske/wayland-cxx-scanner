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
//   SPACE / left-click  toggles the button-active scene state (click the
//   button)
//   F1                  toggles the performance overlay (also --hud)

// ── EGL/GLES headers (before Wayland headers)
// ─────────────────────────────────
extern "C" {
#include <EGL/egl.h>
#include <GLES2/gl2.h>
}

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "presentation_time_client.hpp"  // namespace presentation_time::client
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── System Wayland C headers
// ──────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
}

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/presentation.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>
// The window frame: decoration on a subsurface, driven by a compact wrapper
// that also owns the cursor. Under csd=none it is empty inlines.
#include "decorated_frame.hpp"

// ── Shared scene + pacing
// ────────────────────────────────────────────────────
#include "frame_pacer.hpp"
#include "perf_hud.hpp"
#include "scene.hpp"
#include "view_tree.hpp"

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

// wl_shm — this EGL example renders its content on the GPU and needs no shm of
// its own, but the window frame's cursor loads its theme through one.
class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {};

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  App(demo::PacerConfig pacer_cfg, bool hud) noexcept : pacer_(pacer_cfg) {
    hud_.set_visible(hud);
  }
  ~App();

  int Run();

  void OnXdgSurfaceConfigure(std::uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose() { running_ = false; }
  // The compositor is the authority on these; the frame styles itself from
  // them.
  void OnToplevelStates(const wl::ToplevelStates& states) noexcept {
    frame_.States(states.activated, states.maximized, states.fullscreen);
  }
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameReady(std::uint32_t time_ms) noexcept;
  // Pointer input is delivered by wl::SeatManager; every event goes to the
  // frame, which drives its own title bar, edges and cursor, and the App's hit
  // test runs on top.
  void OnPointerEnter(const wl::PointerEvent& ev) noexcept {
    frame_.PointerEnter(ev, seat_.Pointer());
  }
  void OnPointerLeave() noexcept { frame_.PointerLeave(); }
  void OnPointerMotion(const wl::PointerEvent& ev) noexcept {
    frame_.PointerMotion(ev, seat_.Pointer());
  }
  void OnPointerButton(const wl::PointerButtonEvent& ev) noexcept;
  // A touch tap on the button toggles it too.
  void OnTouchDown(const wl::TouchPoint& p) noexcept;
  // wp_presentation feedback: the frame committed for `fb.frame` turned to
  // light.  wl::PresentationManager creates the feedback when this hook exists.
  void OnPresented(const wl::PresentFeedback& fb) noexcept;

 private:
  static constexpr int kDefaultWidth = 480;
  static constexpr int kDefaultHeight = 320;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitEgl();
  bool MainLoop();
  bool RunSelfPaced();
  void PrintBenchmark() const noexcept;
  void PrintPresentSummary() const noexcept;
  void RequestFrameCallback() noexcept;
  void RenderFrame() noexcept;

  [[nodiscard]] static double NowMs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1.0e6;
  }

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;  // only for the frame's cursor theme
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
  wl::PresentationManager<App> presentation_;
  // The window frame, which also owns the cursor. Declared after surface_ and
  // xdg_toplevel_ so it is destroyed before them.
  wl::csd::DecoratedFrame frame_;
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  bool running_ = true;
  bool configured_ = false;
  int width_ = kDefaultWidth;
  int height_ = kDefaultHeight;

  std::uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  std::uint32_t shm_name_ = 0, shm_ver_ = 0;
  std::uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

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
  // Release GL-backed resources while the EGL context is still current.
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
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    } else if (iface == presentation_time::client::wp_presentation_traits::
                            interface_name) {
      presentation_.Record(name, ver);
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
  // wl_shm is optional — the frame just draws an undecorated-cursor window
  // without it. Bind it when present, for the frame's cursor theme.
  if (shm_name_ != 0) {
    using shm_t = wl_shm_traits;
    if (wl_proxy* raw = registry_.Bind<shm_t>(
            shm_name_, std::min(shm_ver_, shm_t::version)))
      shm_.Attach(raw);
  }
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "skia-egl-canvas: wl_seat bind failed\n");
    return false;
  }
  // wp_presentation is optional; Bind() is a no-op if it was never advertised.
  if (!presentation_.Bind(registry_, this)) {
    std::fprintf(stderr, "skia-egl-canvas: wp_presentation bind failed\n");
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

  // Hand the frame the window; it binds its own globals, settles who decorates,
  // and loads the cursor through the app's shm + compositor. csd=none is inert.
  wl::csd::DecoratedWindow::Config cfg;
  cfg.display = display_.Get();
  cfg.content_surface = surface_.Get()->GetProxy();
  cfg.xdg_surface = xdg_surface_.Get()->GetProxy();
  cfg.xdg_toplevel = xdg_toplevel_.Get()->GetProxy();
  cfg.seat = seat_.Seat();
  cfg.content_width = width_;
  cfg.content_height = height_;
  if (!frame_.Init(cfg, "skia-egl-canvas",
                   shm_.IsNull() ? nullptr : shm_.Get()->GetProxy(),
                   compositor_.Get()->GetProxy())) {
    std::fprintf(stderr, "skia-egl-canvas: window frame failed; undecorated\n");
  }

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
  // A self-paced run must not block on vsync (the surface may never present).
  eglSwapInterval(egl_.display, pacer_.self_paced() ? 0 : 1);

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
  scene_.frame = pacer_.frame();
  fps_.Tick(NowMs());
  if (surface != nullptr) {
    view_tree_.Layout(width_, height_);
    SkCanvas* canvas = surface->getCanvas();
    demo::DemoScene::Render(canvas, scene_, view_tree_);
    // The HUD overlays the scene (no-op while hidden); GL redraws the whole
    // buffer each frame, so no damage bookkeeping is needed.
    hud_.Render(canvas, pacer_, fps_.fps());
    gr_context_->flushAndSubmit();
  }

  // Draw and commit the decoration before eglSwapBuffers commits the content;
  // its subsurface is synchronized, so it reaches the screen with that commit.
  frame_.CommitBeforePresent(width_, height_);
  // eglSwapBuffers performs the wl_surface.commit, so request presentation
  // feedback for this content first — it binds to the commit that follows.
  presentation_.Arm(surface_.Get()->GetProxy(), pacer_.frame());
  eglSwapBuffers(egl_.display, egl_.surface);
  pacer_.Advance();
}

void App::OnPresented(const wl::PresentFeedback& fb) noexcept {
  // Real commit→turn-to-light latency and the compositor's measured refresh.
  pacer_.RecordPresentMs(fb.latency_ms);
  pacer_.NoteRefreshNs(fb.refresh_ns);
}

// Renders a bounded number of frames back-to-back without waiting on compositor
// frame callbacks (swap interval is 0 in this mode), so the run completes even
// when the surface is never presented.
bool App::RunSelfPaced() {
  std::printf("skia-egl-canvas: self-paced run%s\n",
              pacer_.benchmarking() ? " (benchmark)" : "");
  while (running_ && g_running && !pacer_.reached_limit()) {
    const double t0 = NowMs();
    RenderFrame();
    // Time only render + swap; the roundtrip is compositor latency, excluded.
    const double render_ms = NowMs() - t0;
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      std::fprintf(stderr, "skia-egl-canvas: roundtrip failed\n");
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
      "skia-egl-canvas: %zu frames  mean=%.3f ms  p50=%.3f  p95=%.3f  "
      "p99=%.3f\n",
      pacer_.sample_count(), pacer_.Mean(), pacer_.Percentile(50),
      pacer_.Percentile(95), pacer_.Percentile(99));
  PrintPresentSummary();
}

// Reports wp_presentation timing when any frame was actually presented.  In a
// headless or fully occluded run the compositor discards every frame, so this
// stays silent rather than printing zeros.
void App::PrintPresentSummary() const noexcept {
  if (pacer_.present_count() == 0)
    return;
  std::printf(
      "skia-egl-canvas: presentation: %llu shown  latency mean=%.3f ms  "
      "p50=%.3f  p95=%.3f  refresh=%.2f Hz\n",
      static_cast<unsigned long long>(pacer_.present_count()),
      pacer_.PresentMean(), pacer_.PresentPercentile(50),
      pacer_.PresentPercentile(95), pacer_.refresh_hz());
}

bool App::MainLoop() {
  if (pacer_.self_paced())
    return RunSelfPaced();

  std::printf("skia-egl-canvas: press ESC or Ctrl-C to quit\n");
  RequestFrameCallback();
  RenderFrame();
  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; },
      "skia-egl-canvas");
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
  // The configure size is the window geometry (content + decoration); the frame
  // turns it into the content size, answering a zero axis from what it restores
  // to. The EGL window is then resized to the content.
  frame_.ContentSize(w, h, &width_, &height_);
  if (egl_.window != nullptr)
    wl_egl_window_resize(egl_.window, width_, height_, 0, 0);
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
  // The frame gets every button first — it owns the title bar, edges and
  // buttons on its own surface — and hands back only close.
  if (frame_.PointerButton(ev))
    running_ = false;
  if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED || ev.button != BTN_LEFT)
    return;
  if (view_tree_.HitTest(static_cast<SkScalar>(ev.x),
                         static_cast<SkScalar>(ev.y)) == demo::View::kButton) {
    scene_.button_active = !scene_.button_active;
  }
}

void App::OnTouchDown(const wl::TouchPoint& p) noexcept {
  if (view_tree_.HitTest(static_cast<SkScalar>(p.x),
                         static_cast<SkScalar>(p.y)) == demo::View::kButton) {
    scene_.button_active = !scene_.button_active;
  }
}

}  // namespace

namespace {

void PrintUsage() {
  std::printf(
      "usage: skia_egl_canvas [--frames N] [--exit] [--fixed-dt]\n"
      "                       [--benchmark N] [--hud]\n"
      "  --frames N     render at most N frames\n"
      "  --exit         quit once the frame limit is reached\n"
      "  --fixed-dt     deterministic 60 Hz animation clock\n"
      "  --benchmark N  render N frames self-paced and print frame-time "
      "stats\n"
      "  --hud          show the performance overlay (toggle with F1)\n");
}

[[nodiscard]] bool ParseArgs(const std::vector<std::string_view>& args,
                             demo::PacerConfig& cfg,
                             bool& hud) {
  // Parses the next argument as a positive frame count.  Rejecting <= 0 (and
  // absurdly large values) keeps a bounded, self-paced run from looping
  // forever.
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
  if (!ParseArgs(args, cfg, hud)) {
    PrintUsage();
    return EXIT_FAILURE;
  }

  App app(cfg, hud);
  return app.Run();
}
