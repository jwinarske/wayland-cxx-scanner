// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// shadertoy-egl — run any Shadertoy "Image" shader on Wayland via EGL/GLES 3.
//
// Connects to the compositor, creates an XDG toplevel, and renders a Shadertoy
// shader (a file argument, or a built-in default) full-screen with OpenGL ES 3.
// The Wayland/EGL plumbing lives here; all shader handling and GL rendering is
// provided by the platform-agnostic shadertoy-cxx library (shadertoy::Gl-
// Renderer), so the exact same renderer drives the DRM/KMS host in drm-cxx.
//
// Usage:  shadertoy_egl [--cycle N] [shader ...]
//   shader        Shadertoy export .json (multi-pass) or bare Image .frag; with
//                 none, cycle the installed bundled set. SPACE/→ next, ← prev,
//                 mouse/touch drag → iMouse, touch tap → next.
//   --cycle N     Auto-advance every N seconds.
//
// Build requirements: wayland-client, wayland-egl, EGL, GLESv2, shadertoy-cxx.
// Runtime requirement: a Wayland compositor with xdg-shell support.

// ── EGL/GLES headers (must precede Wayland headers on some EGL stacks)
// ────────
extern "C" {
#include <EGL/egl.h>
#include <GLES3/gl3.h>
}

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── System Wayland C headers ─────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>  // KEY_ESC, BTN_LEFT
#include <wayland-client-protocol.h>  // wl_*_interface symbols
#include <wayland-egl.h>              // wl_egl_window_*
}

// ── Framework headers ────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── shadertoy-cxx (platform-agnostic renderer) ───────────────────────────────
#include <shadertoy/gl_renderer.hpp>
#include <shadertoy/inputs.hpp>
#include <shadertoy/loader.hpp>
#include <shadertoy/playlist.hpp>
#include <shadertoy/program.hpp>

// ── Standard library ─────────────────────────────────────────────────────────
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions for the core interfaces this example touches.
// (wl_seat / wl_keyboard come from <wl/seat.hpp>.)
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
const wl_interface& wl_pointer_traits::wl_iface() noexcept {
  return wl_pointer_interface;
}
const wl_interface& wl_touch_traits::wl_iface() noexcept {
  return wl_touch_interface;
}
}  // namespace wayland::client

class App;

// ── Frame-pacing callback
// ─────────────────────────────────────────────────────
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

// ── Pointer → iMouse
// ──────────────────────────────────────────────────────────
class PointerHandler : public wayland::client::CWlPointer<PointerHandler> {
 public:
  App* app_ = nullptr;
  void OnEnter(uint32_t serial,
               wl_proxy* surface,
               wl_fixed_t x,
               wl_fixed_t y) override;
  void OnMotion(uint32_t time, wl_fixed_t x, wl_fixed_t y) override;
  void OnButton(uint32_t serial,
                uint32_t time,
                uint32_t button,
                uint32_t state) override;
};

// ── Touch → iMouse (and tap-to-advance) ──────────────────────────────────────
class TouchHandler : public wayland::client::CWlTouch<TouchHandler> {
 public:
  App* app_ = nullptr;
  void OnDown(uint32_t serial,
              uint32_t time,
              wl_proxy* surface,
              int32_t id,
              wl_fixed_t x,
              wl_fixed_t y) override;
  void OnUp(uint32_t serial, uint32_t time, int32_t id) override;
  void OnMotion(uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) override;
};

// A second wl_seat bound solely to obtain the pointer and touch devices,
// leaving the keyboard to wl::SeatManager.  Binding a global more than once is
// part of the standard registry model and is supported by every compositor.
class PointerSeat : public wayland::client::CWlSeat<PointerSeat> {
 public:
  App* app_ = nullptr;
  void OnCapabilities(uint32_t caps) override;
  void OnName(const char* /*name*/) override {}
};

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════
class App {
 public:
  App(std::vector<std::string> shaders,
      int cycle_seconds,
      std::string media_dir,
      std::string blacklist_path,
      bool audio_enabled)
      : shaders_(std::move(shaders)),
        cycle_seconds_(cycle_seconds),
        media_dir_(std::move(media_dir)),
        blacklist_path_(std::move(blacklist_path)),
        audio_enabled_(audio_enabled) {}
  ~App();

  int Run();

  // Handler callbacks.
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose();
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameReady(uint32_t time_ms) noexcept;

  // Pointer / touch callbacks (called by the input handlers / PointerSeat).
  void CreatePointer();
  void OnPointerMotion(wl_fixed_t x, wl_fixed_t y) noexcept;
  void OnPointerButton(uint32_t button, uint32_t state) noexcept;
  void CreateTouch();
  void OnTouchDown(int32_t id, wl_fixed_t x, wl_fixed_t y) noexcept;
  void OnTouchMotion(int32_t id, wl_fixed_t x, wl_fixed_t y) noexcept;
  void OnTouchUp(int32_t id) noexcept;

 private:
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

  wl::SeatManager<App> seat_;            // keyboard (ESC)
  wl::WlPtr<PointerSeat> pointer_seat_;  // pointer + touch (iMouse)
  wl::WlPtr<PointerHandler> pointer_;
  wl::WlPtr<TouchHandler> touch_;

  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // Renderer + shader state (from shadertoy-cxx).
  shadertoy::GlRenderer renderer_;
  shadertoy::ShaderInputs inputs_;
  shadertoy::Playlist playlist_;
  std::vector<std::string> shaders_;  // shader paths from the CLI
  int cycle_seconds_ = 0;             // auto-advance interval (0 = off)
  std::string media_dir_;       // dir for Shadertoy media (textures/cubemaps)
  std::string blacklist_path_;  // file of shader ids to skip (one per line)
  bool audio_enabled_ = true;   // capture mic for audio (kAudio) channels

  bool running_ = true;
  bool configured_ = false;
  int width_ = 800;
  int height_ = 600;

  // Mouse/touch tracking (pixels, Shadertoy bottom-left origin).
  float mouse_cur_x_ = 0.0f, mouse_cur_y_ = 0.0f;  // iMouse.xy (drag only)
  float mouse_live_x_ = 0.0f,
        mouse_live_y_ = 0.0f;  // live pointer (any motion)
  float mouse_click_x_ = 0.0f, mouse_click_y_ = 0.0f;
  bool mouse_down_ = false;
  int32_t touch_id_ = -1;     // active touch point id, -1 = none
  float touch_moved_ = 0.0f;  // travel since touch-down (for tap detection)

  using Clock = std::chrono::steady_clock;
  Clock::time_point start_{};
  Clock::time_point last_frame_{};
  Clock::time_point
      program_start_{};  // when the current shader began (cycling)

  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t seat_name_ = 0, seat_ver_ = 0;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitEgl();
  bool InitRenderer();
  bool MainLoop();

  void RequestFrameCallback() noexcept;
  void RenderFrame() noexcept;
  bool ApplyCurrent() noexcept;    // SetProgram(current) + reset timing
  bool Advance(int dir) noexcept;  // step in dir, skipping uncompilable shaders
  void Next() noexcept;
  void Prev() noexcept;
  void UpdateInputs() noexcept;
};

// ── Handler bodies needing the full App
// ───────────────────────────────────────
void WlCallbackHandler::OnDone(const uint32_t time_ms) {
  app_->OnFrameReady(time_ms);
}
void PointerHandler::OnEnter(uint32_t /*serial*/,
                             wl_proxy* /*surface*/,
                             wl_fixed_t x,
                             wl_fixed_t y) {
  app_->OnPointerMotion(x, y);
}
void PointerHandler::OnMotion(uint32_t /*time*/, wl_fixed_t x, wl_fixed_t y) {
  app_->OnPointerMotion(x, y);
}
void PointerHandler::OnButton(uint32_t /*serial*/,
                              uint32_t /*time*/,
                              uint32_t button,
                              uint32_t state) {
  app_->OnPointerButton(button, state);
}
void TouchHandler::OnDown(uint32_t /*serial*/,
                          uint32_t /*time*/,
                          wl_proxy* /*surface*/,
                          int32_t id,
                          wl_fixed_t x,
                          wl_fixed_t y) {
  app_->OnTouchDown(id, x, y);
}
void TouchHandler::OnUp(uint32_t /*serial*/, uint32_t /*time*/, int32_t id) {
  app_->OnTouchUp(id);
}
void TouchHandler::OnMotion(uint32_t /*time*/,
                            int32_t id,
                            wl_fixed_t x,
                            wl_fixed_t y) {
  app_->OnTouchMotion(id, x, y);
}
void PointerSeat::OnCapabilities(uint32_t caps) {
  if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0u)
    app_->CreatePointer();
  if ((caps & WL_SEAT_CAPABILITY_TOUCH) != 0u)
    app_->CreateTouch();
}

// ══════════════════════════════════════════════════════════════════════════════
// App implementation
// ══════════════════════════════════════════════════════════════════════════════
App::~App() {
  seat_.Release();
  if (!touch_.IsNull()) {
    using T = wayland::client::wl_touch_traits;
    if (seat_ver_ >= T::Op::Since::Release)
      touch_.Get()->Release();
    touch_.Reset();
  }
  if (!pointer_.IsNull()) {
    using P = wayland::client::wl_pointer_traits;
    if (seat_ver_ >= P::Op::Since::Release)
      pointer_.Get()->Release();
    pointer_.Reset();
  }
  if (!pointer_seat_.IsNull()) {
    using S = wayland::client::wl_seat_traits;
    if (seat_ver_ >= S::Op::Since::Release)
      pointer_seat_.Get()->Release();
    pointer_seat_.Reset();
  }
}

int App::Run() {
  if (!ConnectDisplay() || !ScanGlobals() || !BindGlobals() ||
      !CreateSurfaces() || !InitEgl() || !InitRenderer())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "shadertoy-egl: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "shadertoy-egl: wl_display_get_registry failed\n");
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
      seat_name_ = name;
      seat_ver_ = std::min(ver, wl_s::version);
      seat_.Record(name, ver);
    }
  });
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "shadertoy-egl: timed out waiting for globals\n");
    return false;
  }
  if (!compositor_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr,
                 "shadertoy-egl: missing wl_compositor or xdg_wm_base\n");
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
    std::fprintf(stderr, "shadertoy-egl: wl_compositor bind failed\n");
    return false;
  }

  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "shadertoy-egl: xdg_wm_base bind failed\n");
    return false;
  }

  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "shadertoy-egl: wl_seat bind failed\n");
    return false;
  }
  // Second seat bind for the pointer (optional).
  if (seat_name_ && wl::BindHandler<wl_seat_traits>(registry_, pointer_seat_,
                                                    seat_name_, seat_ver_)) {
    pointer_seat_.Get()->app_ = this;
  }

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "shadertoy-egl: timed out waiting for seat caps\n");
    return false;
  }
  return true;
}

void App::CreatePointer() {
  if (!pointer_.IsNull() || pointer_seat_.IsNull())
    return;
  using namespace wayland::client;
  if (wl::SetupHandler(
          pointer_,
          wl::construct<wl_pointer_traits, wl_seat_traits::Op::GetPointer>(
              *pointer_seat_.Get()))) {
    pointer_.Get()->app_ = this;
  }
}

void App::CreateTouch() {
  if (!touch_.IsNull() || pointer_seat_.IsNull())
    return;
  using namespace wayland::client;
  if (wl::SetupHandler(
          touch_, wl::construct<wl_touch_traits, wl_seat_traits::Op::GetTouch>(
                      *pointer_seat_.Get()))) {
    touch_.Get()->app_ = this;
  }
}

void App::OnTouchDown(int32_t id, wl_fixed_t x, wl_fixed_t y) noexcept {
  if (touch_id_ != -1)
    return;  // track only the first active point
  touch_id_ = id;
  touch_moved_ = 0.0f;
  mouse_cur_x_ = static_cast<float>(wl_fixed_to_double(x));
  mouse_cur_y_ =
      static_cast<float>(height_) - static_cast<float>(wl_fixed_to_double(y));
  mouse_click_x_ = mouse_cur_x_;
  mouse_click_y_ = mouse_cur_y_;
  mouse_down_ = true;
}

void App::OnTouchMotion(int32_t id, wl_fixed_t x, wl_fixed_t y) noexcept {
  if (id != touch_id_)
    return;
  const float nx = static_cast<float>(wl_fixed_to_double(x));
  const float ny =
      static_cast<float>(height_) - static_cast<float>(wl_fixed_to_double(y));
  touch_moved_ += std::abs(nx - mouse_cur_x_) + std::abs(ny - mouse_cur_y_);
  mouse_cur_x_ = nx;
  mouse_cur_y_ = ny;
}

void App::OnTouchUp(int32_t id) noexcept {
  if (id != touch_id_)
    return;
  touch_id_ = -1;
  mouse_down_ = false;
  if (touch_moved_ < 16.0f)  // a tap (little travel) advances the playlist
    Next();
}

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "shadertoy-egl: create_surface failed\n");
    return false;
  }

  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "shadertoy-egl: get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "shadertoy-egl: get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;
  xdg_toplevel_.Get()->SetTitle("shadertoy-egl");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.shadertoy-egl");

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "shadertoy-egl: timed out waiting for configure\n");
    return false;
  }
  return true;
}

bool App::InitEgl() {
  egl_.display = eglGetDisplay(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
      static_cast<EGLNativeDisplayType>(display_.Get()));
  if (egl_.display == EGL_NO_DISPLAY) {
    std::fprintf(stderr, "shadertoy-egl: eglGetDisplay failed\n");
    return false;
  }
  EGLint major = 0, minor = 0;
  if (!eglInitialize(egl_.display, &major, &minor)) {
    std::fprintf(stderr, "shadertoy-egl: eglInitialize failed\n");
    return false;
  }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    std::fprintf(stderr, "shadertoy-egl: eglBindAPI failed\n");
    return false;
  }

  const EGLint cfg_attribs[] = {EGL_SURFACE_TYPE,
                                EGL_WINDOW_BIT,
                                EGL_RENDERABLE_TYPE,
                                EGL_OPENGL_ES3_BIT,
                                EGL_RED_SIZE,
                                8,
                                EGL_GREEN_SIZE,
                                8,
                                EGL_BLUE_SIZE,
                                8,
                                EGL_ALPHA_SIZE,
                                8,
                                EGL_NONE};
  EGLint num_configs = 0;
  if (!eglChooseConfig(egl_.display, cfg_attribs, &egl_.config, 1,
                       &num_configs) ||
      num_configs < 1) {
    std::fprintf(stderr, "shadertoy-egl: eglChooseConfig failed\n");
    return false;
  }

  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  egl_.context =
      eglCreateContext(egl_.display, egl_.config, EGL_NO_CONTEXT, ctx_attribs);
  if (egl_.context == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "shadertoy-egl: eglCreateContext (ES3) failed\n");
    return false;
  }

  egl_.window = wl_egl_window_create(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<wl_surface*>(surface_.Get()->GetProxy()), width_,
      height_);
  if (!egl_.window) {
    std::fprintf(stderr, "shadertoy-egl: wl_egl_window_create failed\n");
    return false;
  }
  egl_.surface = eglCreateWindowSurface(
      egl_.display, egl_.config,
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      reinterpret_cast<EGLNativeWindowType>(egl_.window), nullptr);
  if (egl_.surface == EGL_NO_SURFACE) {
    std::fprintf(stderr, "shadertoy-egl: eglCreateWindowSurface failed\n");
    return false;
  }
  if (!eglMakeCurrent(egl_.display, egl_.surface, egl_.surface, egl_.context)) {
    std::fprintf(stderr, "shadertoy-egl: eglMakeCurrent failed\n");
    return false;
  }
  eglSwapInterval(egl_.display, 1);
  std::printf("shadertoy-egl: EGL %d.%d, OpenGL ES 3 context ready\n", major,
              minor);
  return true;
}

bool App::InitRenderer() {
  if (!media_dir_.empty())
    renderer_.SetMediaDir(media_dir_);

  // Audio (kAudio channels): the renderer captures the default microphone via
  // the compiled audio back-end (PipeWire or ALSA) when one is available.
  // --no-audio disables capture, so audio channels read a silent (black)
  // texture. Must be set before the first program is loaded.
  renderer_.SetAudioEnabled(audio_enabled_);

  // Optional blacklist of shader ids to skip (one id per line, '#' comments) —
  // e.g. produced by gen_blacklist.sh to keep GPU-hanging shaders out of a run.
  std::unordered_set<std::string> blacklist;
  if (!blacklist_path_.empty()) {
    std::ifstream bf(blacklist_path_);
    if (!bf)
      std::fprintf(stderr, "shadertoy-egl: cannot open blacklist %s\n",
                   blacklist_path_.c_str());
    for (std::string line; std::getline(bf, line);) {
      const auto a = line.find_first_not_of(" \t\r\n");
      if (a == std::string::npos || line[a] == '#')
        continue;
      const auto b = line.find_last_not_of(" \t\r\n");
      blacklist.insert(line.substr(a, b - a + 1));
    }
  }
  const auto blacklisted = [&blacklist](const std::string& path) {
    return blacklist.count(std::filesystem::path(path).stem().string()) != 0;
  };

  // Build the playlist: CLI shaders (.json multi-pass / .frag single), else the
  // installed bundled set, else the built-in default.  Blacklisted ids skipped.
  std::string err;
  for (const std::string& s : shaders_) {
    if (blacklisted(s))
      continue;
    if (!playlist_.AddFile(s, &err))
      std::fprintf(stderr, "shadertoy-egl: %s: %s\n", s.c_str(), err.c_str());
  }
  if (playlist_.empty()) {
    const std::string dir = shadertoy::DefaultShaderDir();
    if (blacklist.empty()) {
      playlist_.AddDirectory(dir);
    } else {
      namespace fs = std::filesystem;
      std::error_code ec;
      std::vector<std::string> files;
      for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string ext = e.path().extension().string();
        if ((ext == ".json" || ext == ".frag" || ext == ".glsl") &&
            !blacklisted(e.path().string()))
          files.push_back(e.path().string());
      }
      std::sort(files.begin(), files.end());
      for (const std::string& f : files)
        playlist_.AddFile(f, nullptr);
    }
  }
  if (playlist_.empty())
    playlist_.Add(
        shadertoy::MakeSinglePass(shadertoy::DefaultImageShader(), "default"));

  // Apply the first shader; if it won't compile, skip forward to one that does.
  if (!ApplyCurrent() && !Advance(+1)) {
    std::fprintf(stderr, "shadertoy-egl: no shader could be compiled\n");
    return false;
  }
  std::printf(
      "shadertoy-egl: %zu shader(s) [%s]; cycle=%ds "
      "(SPACE/→ next, ← prev)\n",
      playlist_.size(), playlist_.current().name.c_str(), cycle_seconds_);
  start_ = Clock::now();
  last_frame_ = start_;
  program_start_ = start_;
  return true;
}

bool App::ApplyCurrent() noexcept {
  if (!renderer_.SetProgram(playlist_.current()))
    return false;  // compile/link failed; previous program left intact
  const auto now = Clock::now();
  start_ = now;  // restart iTime for the new shader
  last_frame_ = now;
  program_start_ = now;
  inputs_.frame = 0;
  return true;
}

// Step through the playlist in @p dir (+1 next, -1 prev), skipping shaders that
// fail to compile, until one applies.  Bounded by the playlist size, so an
// all-uncompilable playlist terminates with the previous program still showing.
bool App::Advance(int dir) noexcept {
  for (size_t tried = 0, n = playlist_.size(); tried < n; ++tried) {
    if (dir >= 0)
      playlist_.next();
    else
      playlist_.prev();
    if (ApplyCurrent())
      return true;
    std::fprintf(stderr, "shadertoy-egl: skipping \"%s\" (failed to compile)\n",
                 playlist_.current().name.c_str());
  }
  std::fprintf(stderr, "shadertoy-egl: no compilable shader in playlist\n");
  return false;
}

void App::Next() noexcept {
  if (playlist_.size() > 1)
    Advance(+1);
}

void App::Prev() noexcept {
  if (playlist_.size() > 1)
    Advance(-1);
}

bool App::MainLoop() {
  std::printf("shadertoy-egl: rendering (ESC or close to quit)\n");
  RequestFrameCallback();
  RenderFrame();
  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "shadertoy-egl");
  return ok;
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

void App::RenderFrame() noexcept {
  // Auto-advance the playlist after cycle_seconds_ on the current shader.
  if (cycle_seconds_ > 0 && playlist_.size() > 1) {
    const float since =
        std::chrono::duration<float>(Clock::now() - program_start_).count();
    if (since >= static_cast<float>(cycle_seconds_))
      Next();
  }
  UpdateInputs();
  renderer_.Render(inputs_);
  eglSwapBuffers(egl_.display, egl_.surface);
  ++inputs_.frame;
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

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
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

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;
  switch (ev.key) {
    case KEY_ESC:
      running_ = false;
      break;
    case KEY_SPACE:
    case KEY_RIGHT:
      Next();
      break;
    case KEY_LEFT:
      Prev();
      break;
    default:
      break;
  }
}

void App::OnPointerMotion(wl_fixed_t x, wl_fixed_t y) noexcept {
  mouse_live_x_ = static_cast<float>(wl_fixed_to_double(x));
  // Flip Y to Shadertoy's bottom-left origin.
  mouse_live_y_ =
      static_cast<float>(height_) - static_cast<float>(wl_fixed_to_double(y));
  // Shadertoy only moves iMouse.xy while a button is held (a drag); plain
  // hovering leaves it where the last drag ended.
  if (mouse_down_) {
    mouse_cur_x_ = mouse_live_x_;
    mouse_cur_y_ = mouse_live_y_;
  }
}

void App::OnPointerButton(uint32_t button, uint32_t state) noexcept {
  if (button != BTN_LEFT)
    return;
  mouse_down_ = (state == WL_POINTER_BUTTON_STATE_PRESSED);
  if (mouse_down_) {
    mouse_cur_x_ = mouse_live_x_;  // jump iMouse.xy to the click point
    mouse_cur_y_ = mouse_live_y_;
    mouse_click_x_ = mouse_live_x_;
    mouse_click_y_ = mouse_live_y_;
  }
}

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);

  // CLI:  shadertoy_egl [--cycle N] [shader ...]
  //   shader  — a Shadertoy export .json (multi-pass) or bare Image .frag.
  //   --cycle N — auto-advance every N seconds (default 0 = off). With several
  //               shaders and no --cycle, use SPACE/→/← to switch.
  int cycle_seconds = 0;
  std::string media_dir;
  std::string blacklist_path;
  bool audio_enabled = true;
  std::vector<std::string> shaders;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--cycle" && i + 1 < argc) {
      cycle_seconds = std::atoi(argv[++i]);
    } else if (arg == "--media" && i + 1 < argc) {
      media_dir = argv[++i];
    } else if (arg == "--blacklist" && i + 1 < argc) {
      blacklist_path = argv[++i];
    } else if (arg == "--audio") {
      audio_enabled = true;
    } else if (arg == "--no-audio") {
      audio_enabled = false;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: %s [--cycle N] [--media DIR] [--blacklist FILE] "
          "[--audio|--no-audio] [shader.json|shader.frag ...]\n"
          "  --media DIR      resolve Shadertoy texture/cubemap src under DIR\n"
          "                   (also via $SHADERTOY_MEDIA_DIR)\n"
          "  --blacklist FILE skip shader ids listed in FILE (one per line)\n"
          "  --audio          capture the microphone for audio channels "
          "(default)\n"
          "  --no-audio       disable mic capture (audio channels read "
          "silence)\n",
          argv[0]);
      return EXIT_SUCCESS;
    } else {
      shaders.push_back(arg);
    }
  }
  // Default to cycling the bundled set when run with no arguments.
  if (shaders.empty() && cycle_seconds == 0)
    cycle_seconds = 12;

  App app(std::move(shaders), cycle_seconds, std::move(media_dir),
          std::move(blacklist_path), audio_enabled);
  return app.Run();
}
