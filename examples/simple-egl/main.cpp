// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// simple-egl — color-cycling EGL/OpenGL ES 2 Wayland client.
//
// Connects to the running compositor, creates an XDG toplevel window, and
// renders a hue-shifting solid colour via OpenGL ES 2 + EGL.
//
// Build requirements: wayland-client, wayland-egl, EGL, GLESv2.
// Runtime requirement: a running Wayland compositor with xdg-shell support.

// ── EGL/GLES headers (must precede any Wayland headers to avoid wl_display
//    redefinition issues on some EGL implementations) ─────────────────────────
extern "C" {
#include <EGL/egl.h>
#include <GLES2/gl2.h>
}

// ── Generated C++ protocol headers ───────────────────────────────────────────
// wayland_client.hpp → namespace wayland::client (from wayland.xml)
// xdg_shell_client.hpp → namespace xdg_shell::client (from xdg-shell.xml)
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

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
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── POSIX
// ─────────────────────────────────────────────────────────────────────
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

}  // namespace wayland::client

// xdg-shell wl_interface tables and wl_iface() implementations are provided
// by <wl/xdg_shell.hpp> (already included above).

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
//
// Each handler inherits the generated CRTP base and overrides only the virtual
// event methods it cares about.  Constructor requests (get_xdg_surface,
// get_toplevel, wl_surface.frame, …) are issued at the call site using
// wl::construct<ChildTraits, Opcode>(parent, args…) from proxy_impl.hpp.
//
// wl::XdgWmBaseHandler, wl::XdgSurfaceHandler<App>, and
// wl::XdgToplevelHandler<App> are provided by <wl/xdg_shell.hpp>.
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
// no listener table is needed (and none is generated for fewer interfaces).

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlSurfaceHandler ─────────────────────────────────────────────────────────
// Minimal wl_surface handler.  CWlSurface already provides:
//   • Destroy() — marshals wl_surface.destroy then calls wl_proxy_destroy,
// so WlPtr<WlSurfaceHandler>::Reset() is protocol-correct.
//   • Default no-op overrides for entrance / leave / preferred_buffer_*.
// Nothing to override for this example.

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

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
  //      egl_ → surface_ → compositor_ → registry_ → display_

  // Wayland display — destroyed last so all proxy operations remain valid.
  wl::DisplayHandle display_;

  // Registry — destroyed before display_.
  wl::CRegistry registry_;

  // wl_compositor — destroyed before registry_.  wl_compositor has no
  // protocol destroy request; WlPtr::Reset() calls wl_proxy_destroy directly.
  wl::WlPtr<WlCompositorHandler> compositor_;

  // wl_surface — destroyed before compositor_.  CWlSurface::Destroy() sends
  // wl_surface.destroy then wl_proxy_destroy, so the protocol is satisfied
  // automatically when WlPtr<WlSurfaceHandler>::Reset() fires.
  wl::WlPtr<WlSurfaceHandler> surface_;

  // EGL state — declared after surface_ so its destructor runs before
  // surface_'s, tearing down the EGL window before the wl_surface proxy.
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

  // XDG CRTP handlers — destroyed in reverse: toplevel first, wm_base last.
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // Seat + keyboard manager — keyboard_ inside is destroyed before seat_.
  wl::SeatManager<App> seat_;

  // Frame-pacing callback — destroyed first among all WlPtrs.
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // Application state
  bool running_ = true;
  bool configured_ = false;
  int width_ = 800;
  int height_ = 600;
  uint64_t frame_ = 0;

  // Globals recorded during registry scan
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  /// Maximum time (ms) to wait for a compositor response during startup.
  static constexpr int kRoundtripTimeoutMs = 5000;

  // ── Internal pipeline steps ─────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitEgl();
  /// Run the render loop.  Returns true on a clean exit (user closed window or
  /// pressed ESC), false if the compositor disconnected unexpectedly.
  bool MainLoop();
  // (No CleanupEgl — EglState::~EglState() handles teardown automatically.)

  /// Register a wl_surface.frame callback with the compositor.
  void RequestFrameCallback() noexcept;

  /// Render one frame (GL clear + colour cycle) and swap buffers.
  void RenderFrame() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler method implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

// XDG handler methods are provided by wl::XdgSurfaceHandler<App> and
// wl::XdgToplevelHandler<App> from <wl/xdg_shell.hpp>.
// Seat/keyboard handling is provided by wl::SeatManager<App> from
// <wl/seat.hpp>.

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
  //   frame_callback_ → seat_ (keyboard_ first, then seat_ inside) →
  //   xdg_toplevel_ → xdg_surface_ → xdg_wm_base_ →
  //   egl_ (EglState dtor) → surface_ (wl_surface.destroy) →
  //   compositor_ (wl_proxy_destroy) → registry_ → display_ (disconnect).
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

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "simple-egl: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "simple-egl: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using wl_comp = wayland::client::wl_compositor_traits;
    using xdg_base = xdg_shell::client::xdg_wm_base_traits;
    using wl_s = wayland::client::wl_seat_traits;

    if (iface == wl_comp::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == xdg_base::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_s::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "simple-egl: timed out waiting for global advertisements\n");
    return false;
  }

  if (!compositor_name_) {
    std::fprintf(stderr, "simple-egl: wl_compositor not advertised\n");
    return false;
  }
  if (!xdg_wm_base_name_) {
    std::fprintf(stderr, "simple-egl: xdg_wm_base not advertised\n");
    return false;
  }
  return true;
}

// ── BindGlobals
// ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_compositor — no events, so use Attach() rather than BindHandler() to
  // skip listener installation (CWlCompositor has no s_listener_table_).
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "simple-egl: wl_compositor bind failed\n");
    return false;
  }

  // xdg_wm_base — CRTP handler receives ping events; OnPing calls Pong() only,
  // so no app_ back-pointer is needed.
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "simple-egl: xdg_wm_base bind failed\n");
    return false;
  }

  // wl_seat — optional; SeatManager::Bind() is a no-op if not advertised.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "simple-egl: wl_seat bind failed\n");
    return false;
  }

  // Roundtrip so seat capabilities arrive before CreateSurfaces.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "simple-egl: timed out waiting for seat capabilities\n");
    return false;
  }
  return true;
}

// ── CreateSurfaces
// ────────────────────────────────────────────────────────────

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_compositor.create_surface → wl_surface.
  // wl::construct<> encodes the child interface and opcode at compile time,
  // replacing the raw wl_proxy_marshal_constructor call.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "simple-egl: wl_compositor.create_surface failed\n");
    return false;
  }

  // xdg_wm_base.get_xdg_surface → xdg_surface.
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "simple-egl: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // xdg_surface.get_toplevel → xdg_toplevel.
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "simple-egl: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;

  xdg_toplevel_.Get()->SetTitle("simple-egl");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.simple-egl");

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "simple-egl: timed out waiting for xdg_surface configure\n");
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
    std::fprintf(stderr, "simple-egl: eglGetDisplay failed\n");
    return false;
  }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(egl_.display, &major, &minor)) {
    std::fprintf(stderr, "simple-egl: eglInitialize failed\n");
    return false;
  }
  std::printf("simple-egl: EGL %d.%d initialised\n", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    std::fprintf(stderr, "simple-egl: eglBindAPI(OPENGL_ES) failed\n");
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
    std::fprintf(stderr, "simple-egl: eglChooseConfig failed\n");
    return false;
  }

  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
  static constexpr EGLint kCtxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
  egl_.context = eglCreateContext(egl_.display, egl_.config, EGL_NO_CONTEXT,
                                  std::data(kCtxAttribs));
  if (egl_.context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "simple-egl: eglCreateContext failed\n");
    return false;
  }

  egl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()), width_,
      height_);
  if (!egl_.window) {
    std::fprintf(stderr, "simple-egl: wl_egl_window_create failed\n");
    return false;
  }

  egl_.surface = eglCreateWindowSurface(
      egl_.display, egl_.config,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeWindowType>(egl_.window), nullptr);
  if (egl_.surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "simple-egl: eglCreateWindowSurface failed\n");
    return false;
  }

  if (!eglMakeCurrent(egl_.display, egl_.surface, egl_.surface, egl_.context)) {
    std::fprintf(stderr, "simple-egl: eglMakeCurrent failed\n");
    return false;
  }

  // Request vsync (swap interval 1).  Default is implementation-defined;
  // some EGL platforms default to 0 (unconstrained), causing a busy-loop.
  eglSwapInterval(egl_.display, 1);

  return true;
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

bool App::MainLoop() {
  std::printf("simple-egl: entering render loop (ESC or close to quit)\n");

  // Kickstart: arm the first frame callback, then render and commit frame 0.
  // The compositor will reply with wl_callback.done when it is ready for
  // frame 1, at which point OnFrameReady() takes over the callback chain.
  RequestFrameCallback();
  RenderFrame();  // eglSwapBuffers commits the buffer + the callback request

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "simple-egl");
  if (ok)
    std::printf("simple-egl: exiting cleanly\n");
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
  // wl_surface.frame → wl_callback.  wl::construct<> encodes the child
  // interface and opcode at compile time; the request must arrive before the
  // corresponding wl_surface.commit (implicit in eglSwapBuffers) so that both
  // land in the same compositor message batch.
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_callback_.Get()->app_ = this;
    frame_callback_.Get()->_SetProxy(raw);
  }
}

void App::OnFrameReady(uint32_t /*time_ms*/) noexcept {
  // The compositor has presented the previous frame and is ready for the next.

  // Detach the now-spent wl_callback proxy before arming the next one.
  // Detach() sets m_proxy to nullptr without calling wl_proxy_destroy, so
  // the proxy stays alive through the rest of this dispatch cycle.
  wl_proxy* const spent_cb = frame_callback_.Detach();
  // ScopeExit guarantees wl_proxy_destroy is called on all paths — including
  // any early return added in the future — without an explicit if-destroy at
  // the bottom of the function.
  const auto guard = wl::ScopeExit{[spent_cb] {
    if (spent_cb)
      wl_proxy_destroy(spent_cb);
  }};

  // Arm the next frame callback BEFORE the buffer commit, so the request and
  // commit are delivered to the compositor in the same message batch.
  RequestFrameCallback();

  // Render and commit frame N+1.
  RenderFrame();
}

// ── App callbacks
// ─────────────────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
  // Clamp to a sane upper bound; a compositor bug or malicious value of
  // INT32_MAX would otherwise be forwarded directly to wl_egl_window_resize.
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    width_ = std::min(w, kMaxDim);
    height_ = std::min(h, kMaxDim);
    if (egl_.window)
      wl_egl_window_resize(egl_.window, width_, height_, 0, 0);
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
  // Suppress SIGPIPE so that a compositor disconnect during wl_display_flush
  // is reported as EPIPE / error return rather than terminating the process.
  std::signal(SIGPIPE, SIG_IGN);

  App app;
  return app.Run();
}
