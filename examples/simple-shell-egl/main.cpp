// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// simple-shell-egl — RDK/Westeros simple_shell EGL/OpenGL ES 2 client.
//
// Connects to a compositor implementing the wl_simple_shell protocol, creates
// a wl_surface rendered with a hue-shifting solid color via EGL/GLES2, and
// uses simple_shell to drive its layout: once the compositor assigns a
// surfaceId (delivered through the surface_id event), the client sets the
// surface name, geometry, z-order, opacity and visibility, then issues
// get_status and logs the surface_status events the compositor replies with.
//
// Unlike most desktop clients, simple_shell does not use xdg-shell — surface
// placement is controlled entirely through the shell protocol.
//
// Build requirements: wayland-client, wayland-egl, EGL, GLESv2.
// Runtime requirement: a running compositor advertising wl_simple_shell.

// ── EGL/GLES headers (must precede any Wayland headers to avoid wl_display
//    redefinition issues on some EGL implementations) ─────────────────────────
extern "C" {
#include <EGL/egl.h>
#include <GLES2/gl2.h>
}

// ── Generated C++ protocol headers ───────────────────────────────────────────
// wayland_client.hpp      → namespace wayland::client      (from wayland.xml)
// simple_shell_client.hpp → namespace simple_shell::client (from
// simpleshell.xml)
#include "simple_shell_client.hpp"
#include "wayland_client.hpp"

// ── System Wayland C headers
// ──────────────────────────────────────────────────
extern "C" {
// Provides wl_*_interface symbols used by wl_iface() definitions below.
#include <wayland-client-protocol.h>
// Provides wl_egl_window_{create,destroy,resize}.
#include <wayland-egl.h>
// KEY_ESC and WL_KEYBOARD_KEY_STATE_PRESSED.
#include <linux/input-event-codes.h>
#include <unistd.h>  // close()
}

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/simple_shell.hpp>  // must follow simple_shell_client.hpp
#include <wl/wl_ptr.hpp>

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// <wayland-client-protocol.h> exposes pre-built extern const wl_interface
// symbols for every core Wayland interface.  wl_seat_traits::wl_iface() and
// wl_keyboard_traits::wl_iface() are provided inline by <wl/seat.hpp>;
// wl_simple_shell_traits::wl_iface() is provided by <wl/simple_shell.hpp>.
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

const wl_interface& wl_compositor_traits::wl_iface() noexcept {
  return wl_compositor_interface;
}
const wl_interface& wl_surface_traits::wl_iface() noexcept {
  return wl_surface_interface;
}
const wl_interface& wl_callback_traits::wl_iface() noexcept {
  return wl_callback_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
//
// wl::SimpleShellHandler<App> is provided by <wl/simple_shell.hpp> and
// delegates all six simple_shell events to App.  wl::SeatManager<App>
// (seat + keyboard) is provided by <wl/seat.hpp>.
// ══════════════════════════════════════════════════════════════════════════════

// Forward-declare App so handler callbacks can call back into it.
class App;

// ── WlCallbackHandler ────────────────────────────────────────────────────────
// Handles the one-shot wl_callback.done event used to pace frame production.

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;

  void OnDone(uint32_t time_ms) override;
};

// ── WlCompositorHandler / WlSurfaceHandler ───────────────────────────────────
// Both concrete; no events of interest.

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks invoked by the seat/keyboard manager ──────────────────────
  void OnKey(const wl::KeyEvent& ev);

  // ── Callbacks invoked by wl::SimpleShellHandler<App> ────────────────────
  void OnSimpleShellSurfaceId(wl_proxy* surface, uint32_t surface_id);
  static void OnSimpleShellSurfaceCreated(uint32_t surface_id,
                                          const char* name);
  static void OnSimpleShellSurfaceDestroyed(uint32_t surface_id,
                                            const char* name);
  static void OnSimpleShellSurfaceStatus(uint32_t surface_id,
                                         const char* name,
                                         uint32_t visible,
                                         int32_t x,
                                         int32_t y,
                                         int32_t width,
                                         int32_t height,
                                         wl_fixed_t opacity,
                                         wl_fixed_t zorder);
  static void OnSimpleShellGetSurfacesDone();
  static void OnSimpleShellPopupDetails(uint32_t surface_id,
                                        uint32_t parent_surface_id,
                                        int32_t popup);

  /// Called by WlCallbackHandler::OnDone — render one frame and arm the next.
  void OnFrameReady(uint32_t time_ms) noexcept;

 private:
  // ── Member declaration order determines RAII destruction order.
  //    Destruction sequence (reverse of declaration):
  //      frame_callback_ → seat_ → shell_ → egl_ → surface_ →
  //      compositor_ → registry_ → display_

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlSurfaceHandler> surface_;

  // EGL state — declared after surface_ so it tears down before the surface.
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

  // simple_shell CRTP handler.
  wl::WlPtr<wl::SimpleShellHandler<App>> shell_;

  // Seat + keyboard manager (optional; no-op if no wl_seat advertised).
  wl::SeatManager<App> seat_;

  // Frame-pacing callback — destroyed first among all WlPtrs.
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // Application state
  bool running_ = true;
  int width_ = 640;
  int height_ = 480;
  uint64_t frame_ = 0;
  bool shell_configured_ = false;
  uint32_t surface_id_ = 0;

  // Globals recorded during registry scan
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shell_name_ = 0, shell_ver_ = 0;

  /// Maximum time (ms) to wait for a compositor response during startup.
  static constexpr int kRoundtripTimeoutMs = 5000;

  // ── Internal pipeline steps ─────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurface();
  bool InitEgl();
  bool MainLoop();

  /// Apply the initial layout once the compositor assigns a surfaceId.
  void ConfigureShellSurface() noexcept;

  void RequestFrameCallback() noexcept;
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
  // Versioned seat/keyboard release before member destructors run.
  seat_.Release();
  // Remaining teardown is handled by member destructors in declaration-reverse
  // order (frame_callback_ → seat_ → shell_ → egl_ → surface_ → compositor_ →
  // registry_ → display_).
}

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  if (!CreateSurface())
    return EXIT_FAILURE;
  if (!InitEgl())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "simple-shell-egl: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "simple-shell-egl: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using wl_comp = wayland::client::wl_compositor_traits;
    using shell = simple_shell::client::wl_simple_shell_traits;
    using wl_s = wayland::client::wl_seat_traits;

    if (iface == wl_comp::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == shell::interface_name) {
      shell_name_ = name;
      shell_ver_ = ver;
    } else if (iface == wl_s::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(
        stderr,
        "simple-shell-egl: timed out waiting for global advertisements\n");
    return false;
  }

  if (!compositor_name_) {
    std::fprintf(stderr, "simple-shell-egl: wl_compositor not advertised\n");
    return false;
  }
  if (!shell_name_) {
    std::fprintf(stderr, "simple-shell-egl: wl_simple_shell not advertised\n");
    return false;
  }
  return true;
}

// ── BindGlobals
// ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using simple_shell::client::wl_simple_shell_traits;

  // wl_compositor — no events, so Attach() skips listener installation.
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "simple-shell-egl: wl_compositor bind failed\n");
    return false;
  }

  // wl_simple_shell — CRTP handler receives surface_id / surface_status events.
  if (!wl::BindHandler<wl_simple_shell_traits>(registry_, shell_, shell_name_,
                                               shell_ver_)) {
    std::fprintf(stderr, "simple-shell-egl: wl_simple_shell bind failed\n");
    return false;
  }
  shell_.Get()->app_ = this;

  // wl_seat — optional; Bind() is a no-op if not advertised.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "simple-shell-egl: wl_seat bind failed\n");
    return false;
  }

  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(stderr,
                 "simple-shell-egl: timed out waiting for seat capabilities\n");
    return false;
  }
  return true;
}

// ── CreateSurface
// ─────────────────────────────────────────────────────────────

bool App::CreateSurface() {
  using namespace wayland::client;

  // wl_compositor.create_surface → wl_surface.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "simple-shell-egl: wl_compositor.create_surface failed\n");
    return false;
  }
  return true;
}

// ── InitEgl
// ───────────────────────────────────────────────────────────────────

bool App::InitEgl() {
  egl_.display = eglGetDisplay(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
      static_cast<EGLNativeDisplayType>(display_.Get()));
  if (egl_.display == EGL_NO_DISPLAY) {
    std::fprintf(stderr, "simple-shell-egl: eglGetDisplay failed\n");
    return false;
  }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(egl_.display, &major, &minor)) {
    std::fprintf(stderr, "simple-shell-egl: eglInitialize failed\n");
    return false;
  }
  std::printf("simple-shell-egl: EGL %d.%d initialized\n", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    std::fprintf(stderr, "simple-shell-egl: eglBindAPI(OPENGL_ES) failed\n");
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
      EGL_DEPTH_SIZE,
      24,
      EGL_NONE,
  };
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)

  EGLint num_configs = 0;
  if (!eglChooseConfig(egl_.display, std::data(kConfigAttribs), &egl_.config, 1,
                       &num_configs) ||
      num_configs < 1) {
    std::fprintf(stderr, "simple-shell-egl: eglChooseConfig failed\n");
    return false;
  }

  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
  static constexpr EGLint kCtxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
  egl_.context = eglCreateContext(egl_.display, egl_.config, EGL_NO_CONTEXT,
                                  std::data(kCtxAttribs));
  if (egl_.context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "simple-shell-egl: eglCreateContext failed\n");
    return false;
  }

  egl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()), width_,
      height_);
  if (!egl_.window) {
    std::fprintf(stderr, "simple-shell-egl: wl_egl_window_create failed\n");
    return false;
  }

  egl_.surface = eglCreateWindowSurface(
      egl_.display, egl_.config,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeWindowType>(egl_.window), nullptr);
  if (egl_.surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "simple-shell-egl: eglCreateWindowSurface failed\n");
    return false;
  }

  if (!eglMakeCurrent(egl_.display, egl_.surface, egl_.surface, egl_.context)) {
    std::fprintf(stderr, "simple-shell-egl: eglMakeCurrent failed\n");
    return false;
  }

  eglSwapInterval(egl_.display, 1);
  return true;
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

bool App::MainLoop() {
  std::printf(
      "simple-shell-egl: entering render loop (ESC or close to quit)\n");

  // Kickstart: arm the first frame callback, then render and commit frame 0.
  // The first buffer commit makes the surface known to the compositor, which
  // replies with the surface_id event handled in OnSimpleShellSurfaceId().
  RequestFrameCallback();
  RenderFrame();

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "simple-shell-egl");
  if (ok)
    std::printf("simple-shell-egl: exiting cleanly\n");
  return ok;
}

void App::RenderFrame() noexcept {
  const float r = static_cast<float>(frame_ % 256u) / 255.0f;
  glClearColor(r, 0.3f, 0.5f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(egl_.display, egl_.surface);
  ++frame_;
}

// ── Frame-callback helpers
// ────────────────────────────────────────────────────

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

// ── simple_shell layout
// ───────────────────────────────────────────────────────

void App::ConfigureShellSurface() noexcept {
  if (shell_configured_ || surface_id_ == 0)
    return;
  shell_configured_ = true;

  auto* shell = shell_.Get();
  shell->SetName(surface_id_, "org.wayland-cxx.simple-shell-egl");
  shell->SetGeometry(surface_id_, /*x=*/0, /*y=*/0, width_, height_);
  shell->SetZorder(surface_id_, wl_fixed_from_double(0.5));
  shell->SetOpacity(surface_id_, wl_fixed_from_double(1.0));
  shell->SetVisible(surface_id_, /*visible=*/1);
  // Ask the compositor to report the resulting surface state.
  shell->GetStatus(surface_id_);
}

// ── simple_shell event callbacks
// ──────────────────────────────────────────────

void App::OnSimpleShellSurfaceId(wl_proxy* surface, uint32_t surface_id) {
  // The event may also fire for other clients' surfaces; only react to ours.
  if (surface != surface_.Get()->GetProxy())
    return;
  surface_id_ = surface_id;
  std::printf("simple-shell-egl: assigned surfaceId %u\n", surface_id);
  ConfigureShellSurface();
}

void App::OnSimpleShellSurfaceCreated(uint32_t surface_id, const char* name) {
  std::printf("simple-shell-egl: surface_created id=%u name=%s\n", surface_id,
              name ? name : "");
}

void App::OnSimpleShellSurfaceDestroyed(uint32_t surface_id, const char* name) {
  std::printf("simple-shell-egl: surface_destroyed id=%u name=%s\n", surface_id,
              name ? name : "");
}

void App::OnSimpleShellSurfaceStatus(uint32_t surface_id,
                                     const char* name,
                                     uint32_t visible,
                                     int32_t x,
                                     int32_t y,
                                     int32_t width,
                                     int32_t height,
                                     wl_fixed_t opacity,
                                     wl_fixed_t zorder) {
  std::printf(
      "simple-shell-egl: status id=%u name=%s visible=%u geom=%dx%d@%d,%d "
      "opacity=%.3f zorder=%.3f\n",
      surface_id, name ? name : "", visible, width, height, x, y,
      wl_fixed_to_double(opacity), wl_fixed_to_double(zorder));
}

void App::OnSimpleShellGetSurfacesDone() {
  std::printf("simple-shell-egl: get_surfaces_done\n");
}

void App::OnSimpleShellPopupDetails(uint32_t surface_id,
                                    uint32_t parent_surface_id,
                                    int32_t popup) {
  std::printf("simple-shell-egl: popup_details id=%u parent=%u popup=%d\n",
              surface_id, parent_surface_id, popup);
}

// ── Seat callback
// ─────────────────────────────────────────────────────────────

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.key == KEY_ESC && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  // Suppress SIGPIPE so a compositor disconnect surfaces as EPIPE rather than
  // terminating the process.
  std::signal(SIGPIPE, SIG_IGN);

  App app;
  return app.Run();
}
