// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// subsurfaces — C++23 port of Weston clients/subsurfaces.c
//
// Demonstrates mixed SHM + EGL wl_subsurface rendering:
//   • Main surface (green, SHM)  — XDG toplevel, default 400 × 300
//   • Red subsurface  (desync)  — EGL/OpenGL ES 2 spinning triangle;
//                                   position also oscillates independently
//   • Blue subsurface (sync)    — SHM solid-blue; commits only with parent
//
// Surface geometry mirrors Weston's original:
//   side = min(width, height) / 2
//   red:  (width−side, 0)           size side × (height−side)
//   blue: (width−side, height−side) size side × side
//
// The red subsurface uses an EGL context so it can render GL content.
// A wl_surface.frame callback is registered on red_surface_ before each
// eglSwapBuffers call to deliver the callback via the implicit commit.
// The parent surface (main_surface_) is also committed every frame so
// that wl_subsurface.set_position takes effect (per the Wayland spec).
//
// Controls:
//   Space  — toggle triangle animation (rotation + position oscillation)
//   Up     — shrink window (height −100, min 150)
//   Down   — grow window  (height +100, max 600)
//   Escape — quit

// ── EGL / OpenGL ES 2 headers — must precede any Wayland headers to avoid
//    wl_display redefinition issues on some EGL implementations ─────────────
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
// Provides extern wl_*_interface symbols used by wl_iface() below.
#include <wayland-client-protocol.h>
// wl_egl_window_{create,destroy,resize}
#include <wayland-egl.h>
// KEY_ESC, KEY_SPACE, KEY_UP, KEY_DOWN and WL_KEYBOARD_KEY_STATE_PRESSED.
#include <linux/input-event-codes.h>
// memfd_create, mmap, munmap, ftruncate
#include <sys/mman.h>
#include <unistd.h>  // close()
}

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

// ══════════════════════════════════════════════════════════════════════════════
// OpenGL ES 2 triangle — shader sources and helpers
//
// The red subsurface renders a solid-colored RGB triangle that rotates at
// one radian per second (period ≈ 6.28 s) using a per-frame uniform angle.
// ══════════════════════════════════════════════════════════════════════════════

// Vertex shader: accepts (x,y) position and (r,g,b) color per vertex.
// Applies a 2-D rotation matrix controlled by the u_angle uniform.
static constexpr auto kVertSrc = R"GLSL(
attribute vec2 a_pos;
attribute vec3 a_color;
uniform float u_angle;
varying vec3 v_color;
void main() {
    float c = cos(u_angle);
    float s = sin(u_angle);
    vec2 r = vec2(c * a_pos.x - s * a_pos.y,
                  s * a_pos.x + c * a_pos.y);
    gl_Position = vec4(r, 0.0, 1.0);
    v_color = a_color;
}
)GLSL";

// Fragment shader: outputs the interpolated per-vertex color.
static constexpr const char* kFragSrc = R"GLSL(
precision mediump float;
varying vec3 v_color;
void main() {
    gl_FragColor = vec4(v_color, 1.0);
}
)GLSL";

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
    std::fprintf(stderr, "subsurfaces: shader compile failed:\n%s\n",
                 log.data());
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

/// Link a GLSL ES program from an already-compiled @p vert and @p frag shader.
/// Returns 0 and prints a diagnostic on failure.
static GLuint LinkProgram(const GLuint vert, const GLuint frag) noexcept {
  const GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glLinkProgram(prog);
  GLint ok = GL_FALSE;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    std::array<char, 512> log{};
    glGetProgramInfoLog(prog, static_cast<GLsizei>(log.size()), nullptr,
                        log.data());
    std::fprintf(stderr, "subsurfaces: program link failed:\n%s\n", log.data());
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// <wayland-client-protocol.h> exposes extern const wl_interface symbols for
// every core Wayland interface.
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
const wl_interface& wl_shm_pool_traits::wl_iface() noexcept {
  return wl_shm_pool_interface;
}
const wl_interface& wl_shm_traits::wl_iface() noexcept {
  return wl_shm_interface;
}
const wl_interface& wl_buffer_traits::wl_iface() noexcept {
  return wl_buffer_interface;
}
const wl_interface& wl_subcompositor_traits::wl_iface() noexcept {
  return wl_subcompositor_interface;
}
const wl_interface& wl_subsurface_traits::wl_iface() noexcept {
  return wl_subsurface_interface;
}

}  // namespace wayland::client

// xdg-shell wl_interface tables and wl_iface() implementations are provided
// by <wl/xdg_shell.hpp> (already included above).

// ══════════════════════════════════════════════════════════════════════════════
// Shared-memory helper
//
// Creates an anonymous file via memfd_create, sizes it, maps it, and holds
// both the fd and the mapping alive until the object is destroyed.
// ══════════════════════════════════════════════════════════════════════════════

struct ShmMapping {
  int fd = -1;
  void* data = MAP_FAILED;
  std::size_t size = 0;

  ShmMapping() = default;
  ~ShmMapping() noexcept { Reset(); }
  ShmMapping(const ShmMapping&) = delete;
  ShmMapping& operator=(const ShmMapping&) = delete;
  ShmMapping(ShmMapping&&) = delete;
  ShmMapping& operator=(ShmMapping&&) = delete;

  [[nodiscard]] bool Create(const std::size_t n) noexcept {
    Reset();
    fd = memfd_create("subsurfaces-shm", 0);
    if (fd < 0) {
      std::fprintf(stderr, "subsurfaces: memfd_create: %s\n",
                   std::strerror(errno));
      return false;
    }
    if (ftruncate(fd, static_cast<off_t>(n)) < 0) {
      std::fprintf(stderr, "subsurfaces: ftruncate: %s\n",
                   std::strerror(errno));
      Reset();
      return false;
    }
    data = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
      std::fprintf(stderr, "subsurfaces: mmap: %s\n", std::strerror(errno));
      Reset();
      return false;
    }
    size = n;
    return true;
  }

  void Reset() noexcept {
    if (data != MAP_FAILED) {
      munmap(data, size);
      data = MAP_FAILED;
    }
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
    size = 0;
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

// Forward-declare App so handler callbacks can reach back into it.
class App;

// ── WlCompositorHandler
// ─────────────────────────────────────────────────────── wl_compositor has no
// events — provide the required ProcessEvent stub and use WlPtr::Attach() (not
// _SetProxy) so no listener table is needed.

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlSubcompositorHandler
// ──────────────────────────────────────────────────── wl_subcompositor has no
// events.

class WlSubcompositorHandler
    : public wayland::client::CWlSubcompositor<WlSubcompositorHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlSubsurfaceHandler
// ─────────────────────────────────────────────────────── wl_subsurface has no
// events.

class WlSubsurfaceHandler
    : public wayland::client::CWlSubsurface<WlSubsurfaceHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlShmPoolHandler
// ────────────────────────────────────────────────────────── wl_shm_pool has no
// events — short-lived object used to create buffers.

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlShmHandler
// ──────────────────────────────────────────────────────────────

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  uint32_t formats = 0;
  void OnFormat(const uint32_t fmt) override {
    if (fmt < 32u)
      formats |= (1u << fmt);
  }
};

// ── WlBufferHandler
// ───────────────────────────────────────────────────────────

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool busy = false;
  void OnRelease() override { busy = false; }
};

// ── WlSurfaceHandler ─────────────────────────────────────────────────────────

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// ── WlCallbackHandler
// ─────────────────────────────────────────────────────────

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// ── XDG handlers are provided by <wl/xdg_shell.hpp>.
// ── Seat + keyboard are provided by wl::SeatManager<App> from <wl/seat.hpp>.

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks invoked by CRTP handlers ─────────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose();
  void OnKey(uint32_t key, uint32_t state);
  /// Called by WlCallbackHandler::OnDone — advance the animation.
  void OnFrameReady(uint32_t time_ms) noexcept;

 private:
  // ── Member declaration order = reverse destruction order ───────────────
  //
  // Destruction sequence:
  //   frame_cb_ → seat_ (keyboard_ first, seat_ second inside) →
  //   xdg_toplevel_ → xdg_surface_ → xdg_wm_base_
  //   → blue_subsurface_ → blue_surface_
  //   → gl_ (EGL teardown) → red_subsurface_ → red_surface_
  //   → main_surface_
  //   → subcompositor_ → compositor_ → shm_
  //   → shm_mem_ (munmap + close)
  //   → registry_ → display_

  wl::DisplayHandle display_;

  wl::CRegistry registry_;

  // ── Global bindings (event-less: use Attach, not _SetProxy) ───────────
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<WlSubcompositorHandler> subcompositor_;

  // ── XDG shell ──────────────────────────────────────────────────────────
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;

  // ── Input: seat + keyboard bundled in SeatManager ─────────────────────
  wl::SeatManager<App> seat_;

  // ── SHM memory (declared before buffers and surfaces, so it outlives
  //    everything that holds pointers into it) ────────────────────────────
  ShmMapping shm_mem_;

  // ── SHM buffers (main and blue surfaces only; red uses EGL) ──────────
  wl::WlPtr<WlBufferHandler> main_buf_;
  wl::WlPtr<WlBufferHandler> blue_buf_;

  // ── Surfaces ───────────────────────────────────────────────────────────
  wl::WlPtr<WlSurfaceHandler> main_surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  wl::WlPtr<WlSurfaceHandler> red_surface_;
  wl::WlPtr<WlSubsurfaceHandler> red_subsurface_;

  // ── EGL state for the red subsurface GL triangle ─────────────────────
  // Declared after red_surface_/red_subsurface_, so it is destroyed first
  // (reverse order), ensuring EGL cleanup before the wl_surface proxy goes.
  struct GlState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = {};
    wl_egl_window* window = nullptr;
    GLuint prog = 0;
    GLint a_pos = -1;
    GLint a_color = -1;
    GLint u_angle = -1;

    ~GlState() noexcept {
      if (display == EGL_NO_DISPLAY)
        return;
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (prog)
        glDeleteProgram(prog);
      if (surface != EGL_NO_SURFACE)
        eglDestroySurface(display, surface);
      if (context != EGL_NO_CONTEXT)
        eglDestroyContext(display, context);
      if (window)
        wl_egl_window_destroy(window);
      eglTerminate(display);
    }
    GlState() = default;
    GlState(const GlState&) = delete;
    GlState& operator=(const GlState&) = delete;
  } gl_;

  wl::WlPtr<WlSurfaceHandler> blue_surface_;
  wl::WlPtr<WlSubsurfaceHandler> blue_subsurface_;

  // ── Frame-pacing callback (registered on red_surface_ before each
  //    eglSwapBuffers so the compositor delivers it via the implicit commit)
  wl::WlPtr<WlCallbackHandler> frame_cb_;

  // ── Application state ─────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  bool animate_ = true;

  int width_ = 400;
  int height_ = 300;

  // Accumulated animation time — incremented only while animate_ is true.
  // Both AdvanceAnimation and RenderTriangle use this so that pausing and
  // resuming animation never causes a position or rotation jump.
  uint32_t anim_elapsed_ms_ = 0;
  // Timestamp of the previous frame (used to compute per-frame deltas).
  uint32_t prev_frame_ms_ = 0;

  // Derived geometry (recalculated in ApplyGeometry).
  int side_ = 0;    // subsurface side dimension
  int red_x_ = 0;   // base x of red subsurface
  int red_y_ = 0;   // base y of red subsurface
  int red_w_ = 0;   // width of red subsurface
  int red_h_ = 0;   // height of red subsurface
  int blue_x_ = 0;  // base x of blue subsurface
  int blue_y_ = 0;  // base y of blue subsurface
  int blue_w_ = 0;  // width of blue subsurface
  int blue_h_ = 0;  // height of blue subsurface

  // ── Globals recorded during registry scan ─────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t subcompositor_name_ = 0, subcompositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  static constexpr int kRoundtripTimeoutMs = 5000;

  // ── Pipeline steps ─────────────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool CreateBuffers();
  /// Initialize EGL + compile GL shaders for the red subsurface triangle.
  bool InitGl() noexcept;
  bool InitialCommit();
  [[nodiscard]] bool MainLoop() const;

  void RequestFrameCallback() noexcept;
  void AdvanceAnimation(uint32_t anim_ms) noexcept;
  /// Render one GL triangle frame and swap buffers (commits red_surface_).
  void RenderTriangle(uint32_t anim_ms) noexcept;

  /// Compute subsurface geometry from current width_/height_.
  void ApplyGeometry() noexcept;

  /// (Re)create SHM buffers for main and blue surfaces.
  /// Returns false on allocation failure.
  [[nodiscard]] bool ReallocBuffers() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler method implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

// XDG handler methods are provided by wl::XdgSurfaceHandler<App> and
// wl::XdgToplevelHandler<App> from <wl/xdg_shell.hpp>.
// Seat/keyboard handling is provided by wl::SeatManager<App> from
// <wl/seat.hpp>.

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  app_->OnFrameReady(time_ms);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  seat_.Release();
  // RAII handles all protocol object teardowns in reverse declaration order.
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
  if (!CreateBuffers())
    return EXIT_FAILURE;
  if (!InitGl())
    return EXIT_FAILURE;
  if (!InitialCommit())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "subsurfaces: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "subsurfaces: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, const uint32_t name,
                            const std::string_view iface, const uint32_t ver) {
    using namespace wayland::client;
    using namespace xdg_shell::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == wl_subcompositor_traits::interface_name) {
      subcompositor_name_ = name;
      subcompositor_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "subsurfaces: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_) {
    std::fprintf(stderr, "subsurfaces: wl_compositor not advertised\n");
    return false;
  }
  if (!shm_name_) {
    std::fprintf(stderr, "subsurfaces: wl_shm not advertised\n");
    return false;
  }
  if (!subcompositor_name_) {
    std::fprintf(stderr, "subsurfaces: wl_subcompositor not advertised\n");
    return false;
  }
  if (!xdg_wm_base_name_) {
    std::fprintf(stderr, "subsurfaces: xdg_wm_base not advertised\n");
    return false;
  }
  return true;
}

// ── BindGlobals
// ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_compositor — no events, so Attach() rather than _SetProxy().
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "subsurfaces: wl_compositor bind failed\n");
    return false;
  }

  // wl_shm — has events (format announcements).
  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "subsurfaces: wl_shm bind failed\n");
    return false;
  }

  // wl_subcompositor — no events.
  if (wl_proxy* raw = registry_.Bind<wl_subcompositor_traits>(
          subcompositor_name_,
          std::min(subcompositor_ver_, wl_subcompositor_traits::version))) {
    subcompositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "subsurfaces: wl_subcompositor bind failed\n");
    return false;
  }

  // xdg_wm_base — handles ping events.
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "subsurfaces: xdg_wm_base bind failed\n");
    return false;
  }

  // wl_seat — optional; SeatManager::Bind() is a no-op if not advertised.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "subsurfaces: wl_seat bind failed\n");
    return false;
  }

  // Second roundtrip so that wl_shm format events arrive.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "subsurfaces: timed out waiting for shm formats\n");
    return false;
  }

  // WL_SHM_FORMAT_XRGB8888 = 1 — must be supported.
  constexpr uint32_t kXrgb8888 = 1u;
  if (!(shm_.Get()->formats & (1u << kXrgb8888))) {
    std::fprintf(stderr, "subsurfaces: WL_SHM_FORMAT_XRGB8888 not supported\n");
    return false;
  }
  return true;
}

// ── Geometry helpers
// ──────────────────────────────────────────────────────────

void App::ApplyGeometry() noexcept {
  // Mirror Weston's original layout:
  //   side = min(width, height) / 2
  //   red: (width−side, 0) size side × (height−side)
  //   blue: (width−side, height−side) size side × side
  side_ = std::min(width_, height_) / 2;
  const int remaining_h = height_ - side_;

  red_w_ = side_;
  red_h_ = remaining_h;
  red_x_ = width_ - side_;
  red_y_ = 0;

  blue_w_ = side_;
  blue_h_ = side_;
  blue_x_ = width_ - side_;
  blue_y_ = remaining_h;
}

// ── CreateSurfaces
// ────────────────────────────────────────────────────────────

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // ── Main wl_surface ──────────────────────────────────────────────────────
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    main_surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_compositor.create_surface (main) failed\n");
    return false;
  }

  // ── xdg_surface ──────────────────────────────────────────────────────────
  if (!wl::SetupHandler(
          xdg_surface_,
          wl::construct<xdg_surface_traits,
                        xdg_wm_base_traits::Op::GetXdgSurface>(
              *xdg_wm_base_.Get(), main_surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "subsurfaces: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // ── xdg_toplevel ─────────────────────────────────────────────────────────
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "subsurfaces: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;

  xdg_toplevel_.Get()->SetTitle("Wayland subsurface Demo");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.subsurfaces");
  // Lock the initial size so the compositor doesn't free-size us on startup.
  xdg_toplevel_.Get()->SetMinSize(100, 100);

  // ── Red subsurface ───────────────────────────────────────────────────────
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    red_surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_compositor.create_surface (red) failed\n");
    return false;
  }

  if (wl_proxy* raw = wl::construct<wl_subsurface_traits,
                                    wl_subcompositor_traits::Op::GetSubsurface>(
          *subcompositor_.Get(), red_surface_.Get()->GetProxy(),
          main_surface_.Get()->GetProxy())) {
    red_subsurface_.Attach(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_subcompositor.get_subsurface (red) failed\n");
    return false;
  }
  // Desynchronized: the red subsurface commits independently.
  red_subsurface_.Get()->SetDesync();

  // ── Blue subsurface ──────────────────────────────────────────────────────
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    blue_surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_compositor.create_surface (blue) failed\n");
    return false;
  }

  if (wl_proxy* raw = wl::construct<wl_subsurface_traits,
                                    wl_subcompositor_traits::Op::GetSubsurface>(
          *subcompositor_.Get(), blue_surface_.Get()->GetProxy(),
          main_surface_.Get()->GetProxy())) {
    blue_subsurface_.Attach(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_subcompositor.get_subsurface (blue) "
                 "failed\n");
    return false;
  }
  // Synchronized: the blue subsurface state is applied on the parent commit.
  blue_subsurface_.Get()->SetSync();

  // XDG shell requires an initial empty commit on the main surface, so the
  // compositor sends the mandatory xdg_surface::configure event.  Only after
  // that event is received (and auto-acked by XdgSurfaceHandler::OnConfigure)
  // may the client attach a buffer and commit again.  This mirrors the pattern
  // in examples/simple-egl/main.cpp:CreateSurfaces().
  main_surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "subsurfaces: timed out waiting for initial configure\n");
    return false;
  }
  if (!configured_) {
    std::fprintf(stderr, "subsurfaces: no configure received\n");
    return false;
  }

  return true;
}

// ── CreateBuffers
// ─────────────────────────────────────────────────────────────

bool App::CreateBuffers() {
  ApplyGeometry();
  return ReallocBuffers();
}

bool App::ReallocBuffers() noexcept {
  using namespace wayland::client;

  // Destroy previous buffers, if any.
  main_buf_.Reset();
  blue_buf_.Reset();
  shm_mem_.Reset();

  const std::size_t main_stride = static_cast<std::size_t>(width_) * 4u;
  const std::size_t main_size = main_stride * static_cast<std::size_t>(height_);
  const std::size_t blue_stride = static_cast<std::size_t>(blue_w_) * 4u;
  const std::size_t blue_size = blue_stride * static_cast<std::size_t>(blue_h_);

  // Red subsurface uses EGL — no SHM buffer needed for it.
  const std::size_t total = main_size + blue_size;

  if (!shm_mem_.Create(total)) {
    std::fprintf(stderr, "subsurfaces: SHM allocation failed\n");
    return false;
  }

  // Paint solid colors via a span to avoid raw pointer arithmetic.
  const std::size_t main_px = main_size / sizeof(uint32_t);
  const std::size_t blue_px = blue_size / sizeof(uint32_t);
  const std::span<uint32_t> all{static_cast<uint32_t*>(shm_mem_.data),
                                main_px + blue_px};
  std::fill(all.first(main_px).begin(), all.first(main_px).end(),
            0x0000CC00u);  // Green: XRGB
  std::fill(all.last(blue_px).begin(), all.last(blue_px).end(),
            0x000000CCu);  // Blue: XRGB

  // Create a single pool covering main + blue surfaces.
  wl::WlPtr<WlShmPoolHandler> pool;
  if (wl_proxy* raw =
          wl::construct<wl_shm_pool_traits, wl_shm_traits::Op::CreatePool>(
              *shm_.Get(), static_cast<int32_t>(shm_mem_.fd),
              static_cast<int32_t>(total))) {
    pool.Attach(raw);
  } else {
    std::fprintf(stderr, "subsurfaces: wl_shm.create_pool failed\n");
    return false;
  }

  // Main buffer.
  if (wl_proxy* raw =
          wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
              *pool.Get(), 0, static_cast<int32_t>(width_),
              static_cast<int32_t>(height_), static_cast<int32_t>(main_stride),
              WL_SHM_FORMAT_XRGB8888)) {
    main_buf_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_shm_pool.create_buffer (main) failed\n");
    return false;
  }

  // Blue buffer (starts at offset main_size in the pool).
  if (wl_proxy* raw =
          wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
              *pool.Get(), static_cast<int32_t>(main_size),
              static_cast<int32_t>(blue_w_), static_cast<int32_t>(blue_h_),
              static_cast<int32_t>(blue_stride), WL_SHM_FORMAT_XRGB8888)) {
    blue_buf_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_shm_pool.create_buffer (blue) failed\n");
    return false;
  }

  // Pool is only needed to create buffers; destroy it now.
  pool.Reset();

  return true;
}

// ── InitialCommit
// ─────────────────────────────────────────────────────────────

bool App::InitialCommit() {
  // Position the subsurfaces.
  red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_),
                                     static_cast<int32_t>(red_y_));
  blue_subsurface_.Get()->SetPosition(static_cast<int32_t>(blue_x_),
                                      static_cast<int32_t>(blue_y_));

  // Commit the blue subsurface (SHM).
  blue_surface_.Get()->Attach(blue_buf_.Get()->GetProxy(), 0, 0);
  blue_surface_.Get()->Damage(0, 0, blue_w_, blue_h_);
  blue_surface_.Get()->Commit();
  blue_buf_.Get()->busy = true;

  // Commit the main surface (also applies blue's sync'd position).
  main_surface_.Get()->Attach(main_buf_.Get()->GetProxy(), 0, 0);
  main_surface_.Get()->Damage(0, 0, width_, height_);
  main_surface_.Get()->Commit();
  main_buf_.Get()->busy = true;

  // Render the first GL triangle frame on the red subsurface.
  // RequestFrameCallback() is called inside RenderTriangle() before
  // eglSwapBuffers so the callback request is included in the implicit commit.
  RenderTriangle(0);

  return true;
}

// ── Frame-callback helpers
// ────────────────────────────────────────────────────

void App::RequestFrameCallback() noexcept {
  // Register a wl_surface.frame callback on red_surface_ BEFORE calling
  // eglSwapBuffers.  eglSwapBuffers commits red_surface_ implicitly; because
  // the callback is pending at commit time, the compositor receives both the
  // new buffer and the frame-callback request in one message batch and fires
  // the callback when it next presents the surface.
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw =
          wl::construct<wl_c, wl_s::Op::Frame>(*red_surface_.Get())) {
    frame_cb_.Get()->app_ = this;
    frame_cb_.Get()->_SetProxy(raw);
  }
}

void App::OnFrameReady(const uint32_t time_ms) noexcept {
  // Release the spent callback proxy.
  wl_proxy* const spent = frame_cb_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent)
      wl_proxy_destroy(spent);
  }};

  // Compute per-frame delta and accumulate animation time only while running.
  // This ensures pausing and resuming never causes a position/rotation jump:
  // the angle is always derived from time actually spent animating, not from
  // wall-clock time.
  if (prev_frame_ms_ != 0) {
    const uint32_t delta = time_ms - prev_frame_ms_;
    if (animate_)
      anim_elapsed_ms_ += delta;
  }
  prev_frame_ms_ = time_ms;

  if (animate_)
    AdvanceAnimation(anim_elapsed_ms_);

  // Render the next GL triangle frame.  RenderTriangle() registers a new
  // frame callback on red_surface_ before calling eglSwapBuffers, so the
  // callback is delivered via the implicit commit inside eglSwapBuffers.
  RenderTriangle(anim_elapsed_ms_);

  // Commit the parent surface to apply any wl_subsurface.set_position change
  // from AdvanceAnimation.  Per the Wayland spec, set_position always takes
  // effect on the parent's next commit regardless of the subsurface mode.
  main_surface_.Get()->Commit();
}

void App::AdvanceAnimation(uint32_t anim_ms) noexcept {
  // Oscillate the red subsurface horizontally within its column.
  // Uses the same angle as RenderTriangle so position and triangle spin
  // stay in visual sync: one oscillation ≈ one triangle rotation (≈6.28 s).
  // anim_ms is the accumulated animation time (not wall-clock), so pausing
  // and resuming never causes a position jump.
  const double angle = static_cast<double>(anim_ms) / 1000.0;
  const auto amplitude = static_cast<double>(side_) / 3.0;
  const auto dx = static_cast<int32_t>(amplitude * std::sin(angle));

  red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_) + dx,
                                     static_cast<int32_t>(red_y_));
  // Position takes effect on the parent (main_surface_) commit in OnFrameReady.
}

// ── InitGl
// ────────────────────────────────────────────────────────────────────

bool App::InitGl() noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  gl_.display =
      eglGetDisplay(static_cast<EGLNativeDisplayType>(display_.Get()));
  if (gl_.display == EGL_NO_DISPLAY) {
    std::fprintf(stderr, "subsurfaces: eglGetDisplay failed\n");
    return false;
  }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(gl_.display, &major, &minor)) {
    std::fprintf(stderr, "subsurfaces: eglInitialize failed\n");
    return false;
  }
  std::printf("subsurfaces: EGL %d.%d\n", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    std::fprintf(stderr, "subsurfaces: eglBindAPI(OPENGL_ES) failed\n");
    return false;
  }

  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
  static const EGLint kCfgAttribs[] = {
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
  static constexpr EGLint kCtxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)

  EGLint n = 0;
  if (!eglChooseConfig(gl_.display, std::data(kCfgAttribs), &gl_.config, 1,
                       &n) ||
      n < 1) {
    std::fprintf(stderr, "subsurfaces: eglChooseConfig failed\n");
    return false;
  }

  gl_.context = eglCreateContext(gl_.display, gl_.config, EGL_NO_CONTEXT,
                                 std::data(kCtxAttribs));
  if (gl_.context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "subsurfaces: eglCreateContext failed\n");
    return false;
  }

  // Create an EGL window surface backed by the red wl_surface.
  gl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(red_surface_.Get()->GetProxy()), red_w_,
      red_h_);
  if (!gl_.window) {
    std::fprintf(stderr, "subsurfaces: wl_egl_window_create failed\n");
    return false;
  }

  gl_.surface = eglCreateWindowSurface(
      gl_.display, gl_.config,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeWindowType>(gl_.window), nullptr);
  if (gl_.surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "subsurfaces: eglCreateWindowSurface failed\n");
    return false;
  }

  if (!eglMakeCurrent(gl_.display, gl_.surface, gl_.surface, gl_.context)) {
    std::fprintf(stderr, "subsurfaces: eglMakeCurrent failed\n");
    return false;
  }

  // Unconstrained swap interval: frame pacing is driven by the Wayland
  // wl_surface.frame callback, not by EGL blocking on vsync.
  eglSwapInterval(gl_.display, 0);

  // Compile shaders and link the triangle program.
  const GLuint vert = CompileShader(GL_VERTEX_SHADER, kVertSrc);
  const GLuint frag = CompileShader(GL_FRAGMENT_SHADER, kFragSrc);
  if (!vert || !frag) {
    glDeleteShader(vert);
    glDeleteShader(frag);
    return false;
  }
  gl_.prog = LinkProgram(vert, frag);
  glDeleteShader(vert);
  glDeleteShader(frag);
  if (!gl_.prog)
    return false;

  gl_.a_pos = glGetAttribLocation(gl_.prog, "a_pos");
  gl_.a_color = glGetAttribLocation(gl_.prog, "a_color");
  gl_.u_angle = glGetUniformLocation(gl_.prog, "u_angle");

  std::printf("subsurfaces: GL renderer: %p\n", glGetString(GL_RENDERER));
  return true;
}

// ── RenderTriangle
// ────────────────────────────────────────────────────────────

void App::RenderTriangle(const uint32_t anim_ms) noexcept {
  eglMakeCurrent(gl_.display, gl_.surface, gl_.surface, gl_.context);
  glViewport(0, 0, red_w_, red_h_);

  // Dark background so the colored triangle is clearly visible.
  glClearColor(0.05f, 0.05f, 0.10f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(gl_.prog);

  // Rotation angle: one full turn per ~6.28 seconds (one radian per second).
  // anim_ms is the accumulated animation time, so the triangle freezes
  // while animation is paused and resumes smoothly without jumping.
  glUniform1f(gl_.u_angle, static_cast<float>(anim_ms) / 1000.0f);

  // Equilateral-ish triangle: (x, y, r, g, b) per vertex.
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
  static constexpr float kVerts[] = {
      // x      y      r     g     b
      0.0f,  0.8f,  1.0f, 0.0f, 0.0f,  // top — red
      -0.7f, -0.4f, 0.0f, 1.0f, 0.0f,  // bottom-left — green
      0.7f,  -0.4f, 0.0f, 0.0f, 1.0f,  // bottom-right — blue
  };
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays)

  constexpr GLsizei kStride = 5 * static_cast<GLsizei>(sizeof(float));
  // Compute the two attribute base pointers; wrap each in a lint-suppression
  // block because the violations are on continuation lines (not the first line
  // of the declaration), so NOLINTNEXTLINE would target the wrong line.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* const pos_ptr = reinterpret_cast<const void*>(std::data(kVerts));
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto* const col_ptr =
      reinterpret_cast<const void*>(std::data(kVerts) + 2);
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  glVertexAttribPointer(static_cast<GLuint>(gl_.a_pos), 2, GL_FLOAT, GL_FALSE,
                        kStride, pos_ptr);
  glVertexAttribPointer(static_cast<GLuint>(gl_.a_color), 3, GL_FLOAT, GL_FALSE,
                        kStride, col_ptr);
  glEnableVertexAttribArray(static_cast<GLuint>(gl_.a_pos));
  glEnableVertexAttribArray(static_cast<GLuint>(gl_.a_color));

  glDrawArrays(GL_TRIANGLES, 0, 3);

  // Register the frame callback BEFORE eglSwapBuffers: the callback request
  // is pending on red_surface_, and eglSwapBuffers commits the surface
  // (including all pending Wayland state), so the compositor receives both
  // the new buffer and the callback in one message.
  RequestFrameCallback();

  eglSwapBuffers(gl_.display, gl_.surface);
}

// ── App callbacks
// ─────────────────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    width_ = static_cast<int>(std::min(w, kMaxDim));
    height_ = static_cast<int>(std::min(h, kMaxDim));
    // Recompute subsurface geometry and resize the EGL window so the GL
    // triangle fills the updated red subsurface area.
    ApplyGeometry();
    if (gl_.window)
      wl_egl_window_resize(gl_.window, red_w_, red_h_, 0, 0);
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnKey(const uint32_t key, const uint32_t state) {
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;

  switch (key) {
    case KEY_ESC:
      running_ = false;
      break;

    case KEY_SPACE:
      animate_ = !animate_;
      std::printf("subsurfaces: animation %s\n", animate_ ? "ON" : "OFF");
      break;

    case KEY_UP: {
      if (const int new_h = std::max(150, height_ - 100); new_h != height_) {
        height_ = new_h;
        ApplyGeometry();
        if (ReallocBuffers()) {
          // Resize the EGL window for the red subsurface.
          if (gl_.window)
            wl_egl_window_resize(gl_.window, red_w_, red_h_, 0, 0);
          // Recommit blue and main with new dimensions.
          auto* blue = blue_surface_.Get();
          auto* main_s = main_surface_.Get();
          red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_),
                                             static_cast<int32_t>(red_y_));
          blue_subsurface_.Get()->SetPosition(static_cast<int32_t>(blue_x_),
                                              static_cast<int32_t>(blue_y_));
          blue->Attach(blue_buf_.Get()->GetProxy(), 0, 0);
          blue->Damage(0, 0, blue_w_, blue_h_);
          blue->Commit();
          main_s->Attach(main_buf_.Get()->GetProxy(), 0, 0);
          main_s->Damage(0, 0, width_, height_);
          main_s->Commit();
          xdg_surface_.Get()->SetWindowGeometry(0, 0, width_, height_);
        }
      }
      break;
    }

    case KEY_DOWN: {
      if (const int new_h = std::min(600, height_ + 100); new_h != height_) {
        height_ = new_h;
        ApplyGeometry();
        if (ReallocBuffers()) {
          // Resize the EGL window for the red subsurface.
          if (gl_.window)
            wl_egl_window_resize(gl_.window, red_w_, red_h_, 0, 0);
          auto* blue = blue_surface_.Get();
          auto* main_s = main_surface_.Get();
          red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_),
                                             static_cast<int32_t>(red_y_));
          blue_subsurface_.Get()->SetPosition(static_cast<int32_t>(blue_x_),
                                              static_cast<int32_t>(blue_y_));
          blue->Attach(blue_buf_.Get()->GetProxy(), 0, 0);
          blue->Damage(0, 0, blue_w_, blue_h_);
          blue->Commit();
          main_s->Attach(main_buf_.Get()->GetProxy(), 0, 0);
          main_s->Damage(0, 0, width_, height_);
          main_s->Commit();
          xdg_surface_.Get()->SetWindowGeometry(0, 0, width_, height_);
        }
      }
      break;
    }

    default:
      break;
  }
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

bool App::MainLoop() const {
  std::printf("subsurfaces: entering event loop\n");
  std::printf("  Space  — toggle red subsurface animation\n");
  std::printf("  Up/Down — resize window\n");
  std::printf("  Escape — quit\n");

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "subsurfaces");
  if (ok)
    std::printf("subsurfaces: exiting cleanly\n");
  return ok;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  App app;
  return app.Run();
}
