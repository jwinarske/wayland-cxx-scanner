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
#include "fractional_scale_client.hpp"  // namespace fractional_scale_v1::client
#include "viewporter_client.hpp"        // namespace viewporter::client
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
#include <wl/scale_policy.hpp>  // wl::ScalePolicy — buffer/viewport sizing
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

// wp_viewporter / wp_viewport and the fractional-scale manager have no events.
class WpViewporterHandler
    : public viewporter::client::CWpViewporter<WpViewporterHandler> {};

class WpViewportHandler
    : public viewporter::client::CWpViewport<WpViewportHandler> {};

class WpFractionalScaleManagerHandler
    : public fractional_scale_v1::client::CWpFractionalScaleManagerV1<
          WpFractionalScaleManagerHandler> {};

// wp_fractional_scale_v1 delivers the compositor's preferred scale in 1/120
// units via preferred_scale.
class WpFractionalScaleHandler
    : public fractional_scale_v1::client::CWpFractionalScaleV1<
          WpFractionalScaleHandler> {
 public:
  App* app_ = nullptr;
  void OnPreferredScale(uint32_t scale_120) override;
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
  void OnPreferredScale(int32_t scale_120) noexcept;

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

  wl::WlPtr<WpViewporterHandler> viewporter_;
  wl::WlPtr<WpFractionalScaleManagerHandler> fractional_manager_;

  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // Created from the surface, so destroyed before it.
  wl::WlPtr<WpViewportHandler> viewport_;
  wl::WlPtr<WpFractionalScaleHandler> fractional_scale_;

  wl::WlPtr<WlCallbackHandler> frame_callback_;

  bool running_ = true;
  bool imgui_up_ = false;

  // Logical (surface-local) size, as xdg_toplevel.configure reports it.  The
  // buffer is allocated at physical pixels and the viewport presents it back at
  // this size; ImGui lays out in logical units.
  int width_ = 1024;
  int height_ = 640;

  // Compositor's preferred scale in 1/120 units (unity = 120), the wire format
  // of wp_fractional_scale_v1.preferred_scale.  Unity until told otherwise.
  int32_t scale_120_ = wl::ScalePolicy::kUnityScale120;

  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t viewporter_name_ = 0, viewporter_ver_ = 0;
  uint32_t fractional_name_ = 0, fractional_ver_ = 0;

  // Fractional scale is only honored when a viewport can present the physical
  // buffer at the logical size; without one the buffer would be shown at
  // physical size and the window would be the wrong size on screen.
  [[nodiscard]] bool CanScale() const noexcept {
    return !viewport_.IsNull() && !fractional_scale_.IsNull();
  }
  [[nodiscard]] wl::ScalePolicy::BufferSize BufferPx() const noexcept {
    return wl::ScalePolicy::ToBuffer(width_, height_, scale_120_);
  }
  void ApplyGeometry() noexcept;

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

void WpFractionalScaleHandler::OnPreferredScale(const uint32_t scale_120) {
  app_->OnPreferredScale(static_cast<int32_t>(scale_120));
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
    using wp_vp = viewporter::client::wp_viewporter_traits;
    using wp_fs =
        fractional_scale_v1::client::wp_fractional_scale_manager_v1_traits;
    if (iface == wl_comp::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == xdg_base::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wp_vp::interface_name) {
      viewporter_name_ = name;
      viewporter_ver_ = ver;
    } else if (iface == wp_fs::interface_name) {
      fractional_name_ = name;
      fractional_ver_ = ver;
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

  // Both are optional: without them the window simply stays at unity scale,
  // which is what a compositor that advertises neither is asking for.  They go
  // together — a fractional scale with no viewport to undo it would size the
  // window wrong — so bind the pair or neither.
  // Neither has any events, so they are adopted without a listener.
  if (viewporter_name_ != 0 && fractional_name_ != 0) {
    using wp_vp = viewporter::client::wp_viewporter_traits;
    using wp_fs =
        fractional_scale_v1::client::wp_fractional_scale_manager_v1_traits;
    if (wl_proxy* raw = registry_.Bind<wp_vp>(
            viewporter_name_, std::min(viewporter_ver_, wp_vp::version)))
      viewporter_.Attach(raw);
    if (wl_proxy* raw = registry_.Bind<wp_fs>(
            fractional_name_, std::min(fractional_ver_, wp_fs::version)))
      fractional_manager_.Attach(raw);
    if (viewporter_.IsNull() || fractional_manager_.IsNull()) {
      viewporter_.Reset();
      fractional_manager_.Reset();
    }
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

  // Viewport + fractional scale, when the compositor offered both.  The
  // viewport must exist before the first buffer is attached, or that buffer is
  // presented at physical size for one frame.
  if (!viewporter_.IsNull() && !fractional_manager_.IsNull()) {
    using wp_vp = viewporter::client::wp_viewporter_traits;
    using wp_vps = viewporter::client::wp_viewport_traits;
    using wp_fsm =
        fractional_scale_v1::client::wp_fractional_scale_manager_v1_traits;
    using wp_fs = fractional_scale_v1::client::wp_fractional_scale_v1_traits;

    // wp_viewport has no events either; only wp_fractional_scale_v1 does.
    if (wl_proxy* raw = wl::construct<wp_vps, wp_vp::Op::GetViewport>(
            *viewporter_.Get(), surface_.Get()->GetProxy()))
      viewport_.Attach(raw);
    const bool fs = wl::SetupHandler(
        fractional_scale_,
        wl::construct<wp_fs, wp_fsm::Op::GetFractionalScale>(
            *fractional_manager_.Get(), surface_.Get()->GetProxy()));
    if (!viewport_.IsNull() && fs) {
      fractional_scale_.Get()->app_ = this;
    } else {
      // Half of the pair is useless; fall back to unity rather than present a
      // buffer the compositor would scale itself.
      viewport_.Reset();
      fractional_scale_.Reset();
    }
  }

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

  // The buffer is allocated at physical pixels.  preferred_scale may already
  // have arrived on the configure roundtrip, so take the size from the policy
  // rather than assuming unity.
  const wl::ScalePolicy::BufferSize px0 = BufferPx();
  egl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()), px0.width,
      px0.height);
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
  ImGui_ImplWaylandCxx_SetContentScale(
      static_cast<float>(scale_120_) /
      static_cast<float>(wl::ScalePolicy::kUnityScale120));

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
  // Physical pixels, from the same ScalePolicy that sized the buffer.  Deriving
  // it here as DisplaySize * DisplayFramebufferScale instead would round
  // differently — that product truncates where the policy rounds half up — and
  // at a fractional scale the two disagree by a pixel, leaving a seam along the
  // edge the buffer has but the GL viewport does not cover.
  const wl::ScalePolicy::BufferSize px = BufferPx();
  glViewport(0, 0, static_cast<GLsizei>(px.width),
             static_cast<GLsizei>(px.height));
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

// The single place buffer size, viewport destination and the two ImGui inputs
// are derived from (width_, height_, scale_120_), so they cannot drift apart.
void App::ApplyGeometry() noexcept {
  const wl::ScalePolicy::BufferSize px = BufferPx();
  if (egl_.window != nullptr)
    wl_egl_window_resize(egl_.window, px.width, px.height, 0, 0);

  // The buffer is physical; the viewport presents it back at the logical size,
  // which is what stops the compositor from scaling it a second time.
  if (!viewport_.IsNull())
    viewport_.Get()->SetDestination(width_, height_);

  if (imgui_up_) {
    ImGui_ImplWaylandCxx_SetDisplaySize(width_, height_);
    ImGui_ImplWaylandCxx_SetContentScale(
        static_cast<float>(scale_120_) /
        static_cast<float>(wl::ScalePolicy::kUnityScale120));
  }
}

void App::OnPreferredScale(const int32_t scale_120) noexcept {
  // Honor it only with a viewport to present the physical buffer at the logical
  // size; otherwise the window would come out the wrong size on screen.
  if (!CanScale() || scale_120 <= 0 || scale_120 == scale_120_)
    return;
  scale_120_ = scale_120;
  ApplyGeometry();
}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    width_ = std::min(w, kMaxDim);
    height_ = std::min(h, kMaxDim);
    ApplyGeometry();
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
