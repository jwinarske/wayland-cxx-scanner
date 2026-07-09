// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// agl-presentation-egl — AGL shell background client rendering through an FBO.
//
// Combines two patterns already demonstrated elsewhere in this repo:
//
//   • the AGL shell background-client handshake from agl-presentation-shm
//     (bind agl_shell + xdg_wm_base, empty commit, set_background, wait for
//     configure, ready), and
//   • EGL / OpenGL ES 2 rendering from simple-egl (wl_egl_window + EGL window
//     surface, frame-callback paced eglSwapBuffers).
//
// The distinguishing feature is off-screen rendering via a framebuffer object:
//
//   Pass 1  render an animated, rotating RGB triangle into an offscreen FBO
//           whose color attachment is an ordinary GL_TEXTURE_2D, and
//   Pass 2  bind the default (window) framebuffer and draw a full-screen quad
//           that samples the FBO texture, adding a vignette so the composite
//           pass is visibly distinct from a direct-to-window draw.
//
// This is the canonical GLES2 render-to-texture-then-present flow: GLES2 has
// no glBlitFramebuffer, so the offscreen result is presented by texturing a
// screen-filling quad rather than blitting.
//
// IMPORTANT: set_background must come AFTER the first wl_surface.commit().
// Calling set_background before commit crashes agl-compositor because the
// compositor inspects the committed surface state (role) when processing
// set_background, and an uncommitted surface has no role assigned yet.
//
// Protocol dependencies:
//   xdg-shell.xml  (wayland-protocols, stable)
//   agl-shell.xml  (bundled in protocols/)

// ── EGL/GLES headers (must precede any Wayland headers to avoid wl_display
//    redefinition issues on some EGL implementations) ─────────────────────────
extern "C" {
#include <EGL/egl.h>
#include <GLES2/gl2.h>
}

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "agl_shell_client.hpp"  // namespace agl_shell::client
#include "wayland_client.hpp"    // namespace wayland::client
#include "xdg_shell_client.hpp"  // namespace xdg_shell::client

// ── System Wayland / Linux C headers ─────────────────────────────────────────
extern "C" {
// Provides wl_*_interface symbols used by the wl_iface() definitions below.
#include <wayland-client-protocol.h>
// Provides wl_egl_window_{create,destroy,resize}.
#include <wayland-egl.h>
// KEY_ESC and WL_KEYBOARD_KEY_STATE_PRESSED.
#include <linux/input-event-codes.h>
#include <unistd.h>
}

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/agl_shell.hpp>  // wl_interface tables + wl::AglShellHandler<App>
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>  // wl_interface tables + wl::Xdg*Handler<App>

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// wl_seat_traits / wl_keyboard_traits are provided inline by <wl/seat.hpp>.
// agl_shell_traits by <wl/agl_shell.hpp>; xdg_shell_traits by
// <wl/xdg_shell.hpp>.
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
const wl_interface& wl_output_traits::wl_iface() noexcept {
  return wl_output_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// GLSL ES 1.00 shader sources
// ══════════════════════════════════════════════════════════════════════════════

// Scene pass — a rotating triangle with interpolated per-vertex color.  The
// rotation is applied in-shader from a single u_angle uniform.
static constexpr const char* kSceneVertSrc = R"GLSL(
attribute vec2 a_pos;
attribute vec3 a_color;
uniform float u_angle;
varying vec3 v_color;
void main() {
    float s = sin(u_angle);
    float c = cos(u_angle);
    mat2 rot = mat2(c, -s, s, c);
    gl_Position = vec4(rot * a_pos, 0.0, 1.0);
    v_color = a_color;
}
)GLSL";

static constexpr const char* kSceneFragSrc = R"GLSL(
precision mediump float;
varying vec3 v_color;
void main() {
    gl_FragColor = vec4(v_color, 1.0);
}
)GLSL";

// Composite pass — draw a full-screen quad sampling the offscreen FBO texture,
// darkened toward the edges so the render-to-texture step is visibly present.
static constexpr const char* kCompVertSrc = R"GLSL(
attribute vec2 a_pos;
attribute vec2 a_uv;
varying vec2 v_uv;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
}
)GLSL";

static constexpr const char* kCompFragSrc = R"GLSL(
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
void main() {
    vec3 color = texture2D(u_tex, v_uv).rgb;
    vec2 d = v_uv - 0.5;
    float vignette = smoothstep(0.85, 0.35, dot(d, d) * 2.0);
    gl_FragColor = vec4(color * vignette, 1.0);
}
)GLSL";

// Fixed attribute locations, bound before linking so no glGetAttribLocation
// bookkeeping is needed.  a_pos is location 0 in both programs; the second
// attribute (a_color / a_uv) is location 1.
enum : GLuint { kAttrPos = 0, kAttrExtra = 1 };

/// Compile a GLSL ES shader of the given @p type from @p src.
/// Returns 0 and prints a diagnostic on failure.
static GLuint CompileShader(const GLenum type, const char* src) noexcept {
  const GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = GL_FALSE;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    std::array<char, 512> log{};
    glGetShaderInfoLog(sh, static_cast<GLsizei>(log.size()), nullptr,
                       log.data());
    std::fprintf(stderr, "agl-presentation-egl: shader compile failed:\n%s\n",
                 log.data());
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

/// Compile, bind attribute locations, and link a two-attribute program.
/// @p attr1_name is bound to kAttrExtra; a_pos is always kAttrPos.
/// Returns 0 and prints a diagnostic on failure.
static GLuint BuildProgram(const char* vert_src,
                           const char* frag_src,
                           const char* attr1_name) noexcept {
  const GLuint vert = CompileShader(GL_VERTEX_SHADER, vert_src);
  const GLuint frag = CompileShader(GL_FRAGMENT_SHADER, frag_src);
  if (!vert || !frag) {
    if (vert)
      glDeleteShader(vert);
    if (frag)
      glDeleteShader(frag);
    return 0;
  }

  const GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glBindAttribLocation(prog, kAttrPos, "a_pos");
  glBindAttribLocation(prog, kAttrExtra, attr1_name);
  glLinkProgram(prog);

  // Shaders are no longer needed once linked; flag them for deletion.
  glDeleteShader(vert);
  glDeleteShader(frag);

  GLint ok = GL_FALSE;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    std::array<char, 512> log{};
    glGetProgramInfoLog(prog, static_cast<GLsizei>(log.size()), nullptr,
                        log.data());
    std::fprintf(stderr, "agl-presentation-egl: program link failed:\n%s\n",
                 log.data());
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

class App;

// wl_compositor / wl_surface / wl_output have no events we need; concrete.
class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};
class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};
class WlOutputHandler : public wayland::client::CWlOutput<WlOutputHandler> {};

// wl_callback — one-shot frame-pacing done event.
class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// XDG shell + AGL shell handlers come from <wl/xdg_shell.hpp> and
// <wl/agl_shell.hpp>:
//   wl::XdgWmBaseHandler            — responds to ping automatically
//   wl::XdgSurfaceHandler<App>      — acks configure, calls
//   OnXdgSurfaceConfigure wl::XdgToplevelHandler<App>     — delegates
//   configure/close to App wl::AglShellHandler<App>        — delegates
//   bound_ok/bound_fail/app_state

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks from CRTP handlers ──────────────────────────────────────────
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameReady(uint32_t time_ms) noexcept;

  void OnXdgSurfaceConfigure(uint32_t serial) noexcept;
  void OnToplevelConfigure(int32_t width, int32_t height) noexcept;
  void OnToplevelClose() noexcept;

  void OnAglBoundOk() noexcept;
  void OnAglBoundFail() noexcept;
  static void OnAglAppState(const char* app_id, uint32_t state);

 private:
  // Declaration order = reverse destruction order.  egl_ is declared after
  // surface_ so the EGL window/context is torn down before the wl_surface.
  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlSurfaceHandler> surface_;

  // EGL state — RAII teardown before surface_.
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

  // GL scene objects.  Not torn down explicitly: eglTerminate() in ~EglState()
  // followed by process exit reclaims them, and there is no live context by the
  // time other members are destroyed.
  struct GlState {
    GLuint scene_prog = 0;
    GLuint comp_prog = 0;
    GLint scene_u_angle = -1;
    GLint comp_u_tex = -1;
    GLuint tri_vbo = 0;
    GLuint quad_vbo = 0;
    GLuint fbo = 0;
    GLuint fbo_tex = 0;
    int fbo_w = 0;
    int fbo_h = 0;
  } gl_;

  // XDG shell (required by the AGL compositor for role assignment).
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // AGL shell + the first advertised output (target of set_background).
  wl::WlPtr<wl::AglShellHandler<App>> agl_shell_;
  wl::WlPtr<WlOutputHandler> output_;

  // Seat/keyboard (optional; ESC to quit).
  wl::SeatManager<App> seat_;

  // Frame-pacing callback.
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // Surface dimensions — updated from xdg_toplevel::configure if provided.
  int width_ = 1920;
  int height_ = 1080;
  float angle_ = 0.0f;

  // State flags.
  bool running_ = true;
  bool configured_ = false;

  // agl_shell binding state (v2+ requires waiting for bound_ok/fail).
  enum class BoundState { Waiting, Ok, Fail };
  BoundState bound_state_ = BoundState::Waiting;

  // Registry names/versions.
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t output_name_ = 0, output_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t agl_shell_name_ = 0, agl_shell_ver_ = 0;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool SetupShell();
  bool InitEgl();
  bool InitGl();
  bool MainLoop();

  bool RecreateFbo() noexcept;
  void RequestFrameCallback() noexcept;
  void RenderFrame() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler callbacks (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(const uint32_t time_ms) {
  app_->OnFrameReady(time_ms);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  seat_.Release();
}

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) noexcept {
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t width,
                              const int32_t height) noexcept {
  // The AGL compositor sends the screen dimensions via xdg_toplevel::configure.
  // Clamp to a sane ceiling so a compositor bug can't blow up wl_egl_window or
  // the FBO allocation.
  static constexpr int32_t kMaxDim = 16384;
  if (width > 0 && height > 0) {
    width_ = std::min(width, kMaxDim);
    height_ = std::min(height, kMaxDim);
    if (egl_.window)
      wl_egl_window_resize(egl_.window, width_, height_, 0, 0);
    std::printf("agl-presentation-egl: toplevel configure %dx%d\n", width_,
                height_);
  }
}

void App::OnToplevelClose() noexcept {
  running_ = false;
}

void App::OnAglBoundOk() noexcept {
  bound_state_ = BoundState::Ok;
  std::printf("agl-presentation-egl: bound_ok — shell client accepted\n");
}

void App::OnAglBoundFail() noexcept {
  bound_state_ = BoundState::Fail;
  std::fprintf(stderr,
               "agl-presentation-egl: bound_fail — another AGL shell client is "
               "already active\n");
}

void App::OnAglAppState(const char* app_id, const uint32_t state) {
  std::printf("agl-presentation-egl: app_state app_id=%s state=%u\n", app_id,
              state);
}

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  // SetupShell must run before InitEgl so xdg_toplevel::configure can update
  // width_/height_ before the EGL window and FBO are sized.
  if (!SetupShell())
    return EXIT_FAILURE;
  if (!InitEgl())
    return EXIT_FAILURE;
  if (!InitGl())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "agl-presentation-egl: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr,
                 "agl-presentation-egl: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, const uint32_t name,
                            const std::string_view iface, const uint32_t ver) {
    using namespace wayland::client;
    using namespace xdg_shell::client;
    using namespace agl_shell::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_output_traits::interface_name && !output_name_) {
      output_name_ = name;
      output_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == agl_shell_traits::interface_name) {
      agl_shell_name_ = name;
      agl_shell_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "agl-presentation-egl: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_) {
    std::fprintf(stderr,
                 "agl-presentation-egl: wl_compositor not advertised\n");
    return false;
  }
  if (!xdg_wm_base_name_) {
    std::fprintf(stderr,
                 "agl-presentation-egl: xdg_wm_base not advertised "
                 "(compositor missing xdg-shell support?)\n");
    return false;
  }
  if (!agl_shell_name_) {
    std::fprintf(stderr,
                 "agl-presentation-egl: agl_shell not advertised "
                 "(not an AGL compositor?)\n");
    return false;
  }
  if (!output_name_) {
    std::fprintf(stderr, "agl-presentation-egl: no wl_output advertised\n");
    return false;
  }
  return true;
}

// ── BindGlobals
// ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace agl_shell::client;

  // wl_compositor — no events; Attach() to skip listener installation.
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "agl-presentation-egl: wl_compositor bind failed\n");
    return false;
  }

  // wl_output — no events we care about; Attach().
  if (wl_proxy* raw = registry_.Bind<wl_output_traits>(
          output_name_, std::min(output_ver_, wl_output_traits::version))) {
    output_.Attach(raw);
  } else {
    std::fprintf(stderr, "agl-presentation-egl: wl_output bind failed\n");
    return false;
  }

  // xdg_wm_base — required for surface role assignment.
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "agl-presentation-egl: xdg_wm_base bind failed\n");
    return false;
  }

  // agl_shell — install the event listener before the roundtrip so bound_ok /
  // bound_fail (since v2) arrive during it.
  if (!wl::BindHandler<agl_shell_traits>(registry_, agl_shell_, agl_shell_name_,
                                         agl_shell_ver_)) {
    std::fprintf(stderr, "agl-presentation-egl: agl_shell bind failed\n");
    return false;
  }
  agl_shell_.Get()->app_ = this;  // needed to dispatch bound_ok/fail

  // wl_seat (optional; provides keyboard for ESC-to-quit).
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "agl-presentation-egl: wl_seat bind failed\n");
    return false;
  }

  // Roundtrip so seat capabilities and agl_shell.bound_ok/bound_fail (v2+)
  // arrive.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "agl-presentation-egl: timed out waiting for bind events\n");
    return false;
  }

  // Verify agl_shell binding was accepted (required for protocol v2+).  A v1
  // compositor sends no bound event, so Waiting is treated as Ok.
  if (bound_state_ == BoundState::Fail)
    return false;
  if (bound_state_ == BoundState::Waiting) {
    const uint32_t bound_ver =
        std::min(agl_shell_ver_, agl_shell_traits::version);
    if (bound_ver >= 2u) {
      std::fprintf(stderr,
                   "agl-presentation-egl: no bound event received from v%u "
                   "compositor (compositor bug?)\n",
                   bound_ver);
      return false;
    }
    std::printf(
        "agl-presentation-egl: v1 compositor — proceeding without bound "
        "confirmation\n");
  }
  return true;
}

// ── SetupShell
// ────────────────────────────────────────────────────────────────
//
// Canonical xdg + agl_shell background-surface sequence.  Critical ordering:
//   1. wl_surface + xdg_surface + xdg_toplevel creation
//   2. wl_surface.commit() — MUST come first; establishes the committed role
//      the compositor inspects on set_background
//   3. agl_shell.set_background(surface, output) — AFTER commit
//   4. dispatch until xdg_surface::configure → ack_configure
//   5. agl_shell.ready()

bool App::SetupShell() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // 1. Create the wl_surface used as the background.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "agl-presentation-egl: wl_compositor.create_surface failed\n");
    return false;
  }

  // 2a. Wrap the wl_surface in an xdg_surface.
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr,
                 "agl-presentation-egl: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // 2b. Promote the xdg_surface to a toplevel.
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr,
                 "agl-presentation-egl: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;
  xdg_toplevel_.Get()->SetTitle("agl-presentation-egl");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.agl-presentation-egl");

  // 3. Empty commit — establishes the xdg role in the committed state.  MUST
  //    precede set_background.
  surface_.Get()->Commit();

  // 4. Register this surface as the background for the first output.
  agl_shell_.Get()->SetBackground(surface_.Get()->GetProxy(),
                                  output_.Get()->GetProxy());
  std::printf(
      "agl-presentation-egl: background surface registered with agl_shell\n");

  // 5. Dispatch until xdg_surface::configure arrives (ack'd automatically by
  //    XdgSurfaceHandler → OnXdgSurfaceConfigure → configured_ = true).
  while (!configured_) {
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      std::fprintf(stderr,
                   "agl-presentation-egl: timed out waiting for xdg_surface "
                   "configure\n");
      return false;
    }
  }
  std::printf("agl-presentation-egl: xdg_surface configured (%dx%d)\n", width_,
              height_);

  // 6. Signal the compositor the shell client is fully initialized.
  agl_shell_.Get()->Ready();
  std::printf("agl-presentation-egl: agl_shell.ready sent\n");
  return true;
}

// ── InitEgl
// ───────────────────────────────────────────────────────────────────

bool App::InitEgl() {
  egl_.display = eglGetDisplay(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
      static_cast<EGLNativeDisplayType>(display_.Get()));
  if (egl_.display == EGL_NO_DISPLAY) {
    std::fprintf(stderr, "agl-presentation-egl: eglGetDisplay failed\n");
    return false;
  }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(egl_.display, &major, &minor)) {
    std::fprintf(stderr, "agl-presentation-egl: eglInitialize failed\n");
    return false;
  }
  std::printf("agl-presentation-egl: EGL %d.%d initialized\n", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    std::fprintf(stderr,
                 "agl-presentation-egl: eglBindAPI(OPENGL_ES) failed\n");
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
      EGL_NONE,
  };
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)

  EGLint num_configs = 0;
  if (!eglChooseConfig(egl_.display, std::data(kConfigAttribs), &egl_.config, 1,
                       &num_configs) ||
      num_configs < 1) {
    std::fprintf(stderr, "agl-presentation-egl: eglChooseConfig failed\n");
    return false;
  }

  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
  static constexpr EGLint kCtxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)
  egl_.context = eglCreateContext(egl_.display, egl_.config, EGL_NO_CONTEXT,
                                  std::data(kCtxAttribs));
  if (egl_.context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "agl-presentation-egl: eglCreateContext failed\n");
    return false;
  }

  egl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()), width_,
      height_);
  if (!egl_.window) {
    std::fprintf(stderr, "agl-presentation-egl: wl_egl_window_create failed\n");
    return false;
  }

  egl_.surface = eglCreateWindowSurface(
      egl_.display, egl_.config,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeWindowType>(egl_.window), nullptr);
  if (egl_.surface == EGL_NO_SURFACE) {
    std::fprintf(stderr,
                 "agl-presentation-egl: eglCreateWindowSurface failed\n");
    return false;
  }

  if (!eglMakeCurrent(egl_.display, egl_.surface, egl_.surface, egl_.context)) {
    std::fprintf(stderr, "agl-presentation-egl: eglMakeCurrent failed\n");
    return false;
  }

  // Request vsync so the render loop is display-paced rather than a busy loop.
  eglSwapInterval(egl_.display, 1);
  return true;
}

// ── InitGl
// ────────────────────────────────────────────────────────────────────
//
// Build both programs, upload the static geometry, and create the offscreen
// FBO sized to the current surface.

bool App::InitGl() {
  gl_.scene_prog = BuildProgram(kSceneVertSrc, kSceneFragSrc, "a_color");
  gl_.comp_prog = BuildProgram(kCompVertSrc, kCompFragSrc, "a_uv");
  if (!gl_.scene_prog || !gl_.comp_prog)
    return false;

  gl_.scene_u_angle = glGetUniformLocation(gl_.scene_prog, "u_angle");
  gl_.comp_u_tex = glGetUniformLocation(gl_.comp_prog, "u_tex");

  // Scene triangle: interleaved position (vec2) + color (vec3), RGB corners.
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
  static const GLfloat kTriData[] = {
      0.0f,   0.75f,  1.0f, 0.0f, 0.0f,  // top    — red
      -0.75f, -0.65f, 0.0f, 1.0f, 0.0f,  // left   — green
      0.75f,  -0.65f, 0.0f, 0.0f, 1.0f,  // right  — blue
  };
  // Full-screen quad: interleaved clip position (vec2) + uv (vec2),
  // TRIANGLE_STRIP order.
  static const GLfloat kQuadData[] = {
      -1.0f, -1.0f, 0.0f, 0.0f,  //
      1.0f,  -1.0f, 1.0f, 0.0f,  //
      -1.0f, 1.0f,  0.0f, 1.0f,  //
      1.0f,  1.0f,  1.0f, 1.0f,  //
  };
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)

  glGenBuffers(1, &gl_.tri_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gl_.tri_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kTriData), std::data(kTriData),
               GL_STATIC_DRAW);

  glGenBuffers(1, &gl_.quad_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gl_.quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadData), std::data(kQuadData),
               GL_STATIC_DRAW);

  if (!RecreateFbo())
    return false;

  std::printf("agl-presentation-egl: GL initialized, offscreen FBO %dx%d\n",
              gl_.fbo_w, gl_.fbo_h);
  return true;
}

// ── RecreateFbo
// ───────────────────────────────────────────────────────────────
//
// Create (or re-create after a resize) the offscreen framebuffer whose color
// attachment is a GL_TEXTURE_2D sampled by the composite pass.

bool App::RecreateFbo() noexcept {
  if (gl_.fbo_tex)
    glDeleteTextures(1, &gl_.fbo_tex);
  if (gl_.fbo)
    glDeleteFramebuffers(1, &gl_.fbo);

  gl_.fbo_w = width_;
  gl_.fbo_h = height_;

  glGenTextures(1, &gl_.fbo_tex);
  glBindTexture(GL_TEXTURE_2D, gl_.fbo_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gl_.fbo_w, gl_.fbo_h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &gl_.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, gl_.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         gl_.fbo_tex, 0);

  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr,
                 "agl-presentation-egl: framebuffer incomplete (0x%04x)\n",
                 status);
    return false;
  }
  return true;
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

bool App::MainLoop() {
  std::printf(
      "agl-presentation-egl: running as background shell client (ESC to "
      "quit)\n");

  // Kickstart: arm the first frame callback, then render + swap frame 0.  The
  // compositor replies with wl_callback.done when ready for the next frame,
  // handing the callback chain to OnFrameReady().
  RequestFrameCallback();
  RenderFrame();

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "agl-presentation-egl");
  if (ok)
    std::printf("agl-presentation-egl: exiting cleanly\n");
  return ok;
}

// ── RenderFrame
// ───────────────────────────────────────────────────────────────

void App::RenderFrame() noexcept {
  // Keep the offscreen FBO matched to the current surface size.
  if (gl_.fbo_w != width_ || gl_.fbo_h != height_)
    RecreateFbo();

  // ── Pass 1: render the animated scene into the offscreen FBO ──────────────
  glBindFramebuffer(GL_FRAMEBUFFER, gl_.fbo);
  glViewport(0, 0, gl_.fbo_w, gl_.fbo_h);
  glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(gl_.scene_prog);
  glUniform1f(gl_.scene_u_angle, angle_);
  glBindBuffer(GL_ARRAY_BUFFER, gl_.tri_vbo);
  constexpr GLsizei kTriStride = 5 * sizeof(GLfloat);
  glEnableVertexAttribArray(kAttrPos);
  glVertexAttribPointer(kAttrPos, 2, GL_FLOAT, GL_FALSE, kTriStride, nullptr);
  glEnableVertexAttribArray(kAttrExtra);
  glVertexAttribPointer(
      kAttrExtra, 3, GL_FLOAT, GL_FALSE, kTriStride,
      // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisableVertexAttribArray(kAttrPos);
  glDisableVertexAttribArray(kAttrExtra);

  // ── Pass 2: composite the FBO texture onto the window framebuffer ─────────
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width_, height_);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(gl_.comp_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl_.fbo_tex);
  glUniform1i(gl_.comp_u_tex, 0);
  glBindBuffer(GL_ARRAY_BUFFER, gl_.quad_vbo);
  constexpr GLsizei kQuadStride = 4 * sizeof(GLfloat);
  glEnableVertexAttribArray(kAttrPos);
  glVertexAttribPointer(kAttrPos, 2, GL_FLOAT, GL_FALSE, kQuadStride, nullptr);
  glEnableVertexAttribArray(kAttrExtra);
  glVertexAttribPointer(
      kAttrExtra, 2, GL_FLOAT, GL_FALSE, kQuadStride,
      // NOLINTNEXTLINE(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(kAttrPos);
  glDisableVertexAttribArray(kAttrExtra);

  eglSwapBuffers(egl_.display, egl_.surface);
  angle_ += 0.02f;
}

// ── Frame-callback helpers
// ────────────────────────────────────────────────────

void App::RequestFrameCallback() noexcept {
  // wl_surface.frame → wl_callback.  Must be issued before the commit implicit
  // in eglSwapBuffers so both land in the same compositor message batch.
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_callback_.Get()->app_ = this;
    frame_callback_.Get()->_SetProxy(raw);
  }
}

void App::OnFrameReady(uint32_t /*time_ms*/) noexcept {
  // Detach the spent wl_callback before arming the next one; ScopeExit destroys
  // it on every path.
  wl_proxy* const spent_cb = frame_callback_.Detach();
  const auto guard = wl::ScopeExit{[spent_cb] {
    if (spent_cb)
      wl_proxy_destroy(spent_cb);
  }};

  RequestFrameCallback();
  RenderFrame();
}

// ── App callbacks
// ─────────────────────────────────────────────────────────────

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.key == KEY_ESC && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  App app;
  return app.Run();
}
