// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// agl-compositor — C++23 AGL shell background client example
//
// Demonstrates the correct pattern for an AGL compositor client as used
// by a production AGL shell client:
//
//   1. Bind both xdg_wm_base and agl_shell from the registry.
//   2. Wait for agl_shell.bound_ok / bound_fail (v2+).
//   3. Create wl_surface → xdg_surface → xdg_toplevel.
//   4. Set app_id and title on the toplevel.
//   5. Do an empty wl_surface.commit() — establishes the committed surface
//      state (xdg role) that the compositor inspects on set_background.
//   6. Call agl_shell.set_background(surface, output) AFTER the commit.
//   7. Wait for xdg_surface::configure → ack_configure.
//   8. Call agl_shell.ready() to signal the compositor.
//   9. Allocate SHM buffers and begin frame rendering.
//
// IMPORTANT: set_background must come AFTER the first wl_surface.commit().
// Calling set_background before commit crashes agl-compositor because the
// compositor inspects the committed surface state (role) when processing
// set_background, and an uncommitted surface has no role assigned yet.
//
// The surface draws an animated color-cycling pattern at ~60 fps.
//
// Protocol dependencies:
//   xdg-shell.xml  (wayland-protocols, stable)
//   agl-shell.xml  (bundled in protocols/)

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "agl_shell_client.hpp"  // namespace agl_shell::client
#include "wayland_client.hpp"    // namespace wayland::client
#include "xdg_shell_client.hpp"  // namespace xdg_shell::client

// ── System Wayland / Linux C headers ─────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
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
#include <cassert>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <wl/span.hpp>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() — core Wayland interfaces
//
// The wl_iface() definitions for every interface SeatManager binds (wl_seat,
// wl_keyboard, wl_pointer, wl_touch) are provided inline by <wl/seat.hpp>.
// agl_shell_traits::wl_iface() and the agl_shell interface tables come from the
// generated agl_shell_client.hpp, which is built with --emit-interface-tables
// because agl_shell has no pre-built system symbol.
// All xdg_shell_traits::wl_iface() implementations are provided inline by
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
const wl_interface& wl_shm_pool_traits::wl_iface() noexcept {
  return wl_shm_pool_interface;
}
const wl_interface& wl_shm_traits::wl_iface() noexcept {
  return wl_shm_interface;
}
const wl_interface& wl_buffer_traits::wl_iface() noexcept {
  return wl_buffer_interface;
}
const wl_interface& wl_output_traits::wl_iface() noexcept {
  return wl_output_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// Shared-memory helper
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

  [[nodiscard]] bool Create(std::size_t n) noexcept {
    Reset();
    fd = memfd_create("agl-compositor-bg", 0);
    if (fd < 0)
      return false;
    if (ftruncate(fd, static_cast<off_t>(n)) < 0) {
      Reset();
      return false;
    }
    data = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
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
// Pixel painting
// ══════════════════════════════════════════════════════════════════════════════

/// Fill the @p image (XRGB8888) with an animated color wheel.
static void paint_pixels(wl::span<uint32_t> buf,
                         const int width,
                         const int height,
                         const uint32_t phase) noexcept {
  const int halfh = height / 2;
  const int halfw = width / 2;

  const double ang = M_PI * 2.0 / 1'000'000.0 * static_cast<double>(phase);
  const double s = std::sin(ang);
  const double c = std::cos(ang);

  const int64_t outer_r_sq = [&] {
    const int64_t r = (halfw < halfh ? halfw : halfh) - 16;
    return r * r;
  }();

  for (int y = 0; y < height; ++y) {
    const int oy = y - halfh;
    const int64_t y2 = static_cast<int64_t>(oy) * oy;
    for (int x = 0; x < width; ++x) {
      const int ox = x - halfw;
      const std::size_t idx =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);
      if (static_cast<int64_t>(ox) * ox + y2 > outer_r_sq) {
        buf[idx] = (ox * oy > 0) ? 0xFF000000u : 0xFFFFFFFFu;
        continue;
      }
      const double rx = c * ox + s * oy;
      const double ry = -s * ox + c * oy;
      uint32_t v = 0xFF000000u;
      if (rx < 0.0)
        v |= 0x00FF0000u;
      if (ry < 0.0)
        v |= 0x0000FF00u;
      if ((rx < 0.0) == (ry < 0.0))
        v |= 0x000000FFu;
      buf[idx] = v;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

class App;

// ── WlCompositorHandler
// ───────────────────────────────────────────────────────

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
};

// ── WlShmPoolHandler
// ──────────────────────────────────────────────────────────

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
 public:
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

// ── WlOutputHandler
// ─────────────────────────────────────────────────────────── Minimal wl_output
// handler; we only need the proxy for set_background.

class WlOutputHandler : public wayland::client::CWlOutput<WlOutputHandler> {};

// ── WlCallbackHandler
// ─────────────────────────────────────────────────────────

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// ── XDG shell handlers provided by <wl/xdg_shell.hpp> ────────────────────────
//   wl::XdgWmBaseHandler — responds to ping automatically
//   wl::XdgSurfaceHandler<App> — acks `configure`, calls
//   App::OnXdgSurfaceConfigure wl::XdgToplevelHandler<App> — delegates
//   configure/close to App

// ── AglShellHandler from <wl/agl_shell.hpp> ──────────────────────────────────
//   wl::AglShellHandler<App> — delegates bound_ok/bound_fail/app_state to
//   App

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks from CRTP handlers ──────────────────────────────────────────
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameDone(uint32_t time_ms) noexcept;

  /// xdg_surface::configure received (AckConfigure already done by handler).
  void OnXdgSurfaceConfigure(uint32_t serial) noexcept;
  /// xdg_toplevel::configure received — update dimensions if the compositor
  /// specified them.
  void OnToplevelConfigure(int32_t width, int32_t height) noexcept;
  /// xdg_toplevel::close received — quit cleanly.
  void OnToplevelClose() noexcept;

  /// Called by wl::AglShellHandler<App>::OnBoundOk — compositor accepted
  /// binding.
  void OnAglBoundOk() noexcept;
  /// Called by wl::AglShellHandler<App>::OnBoundFail — another shell active.
  void OnAglBoundFail() noexcept;
  /// Called by wl::AglShellHandler<App>::OnAppState — app lifecycle event.
  static void OnAglAppState(const char* app_id, uint32_t state);

 private:
  // Declaration order = reverse destruction order.
  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  // Core Wayland objects
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<WlSurfaceHandler> surface_;

  // XDG shell (required by AGL compositor)
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // AGL shell
  wl::WlPtr<wl::AglShellHandler<App>> agl_shell_;

  // Output — we bind the first advertised wl_output
  wl::WlPtr<WlOutputHandler> output_;

  // Seat/keyboard (optional; ESC to quit)
  wl::SeatManager<App> seat_;

  // Frame-pacing callback
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // SHM backing store — two buffers for double-buffering.
  // Initial dimensions; updated from xdg_toplevel::configure if provided.
  int width_ = 1920;
  int height_ = 1080;
  static constexpr int kNumBufs = 2;

  ShmMapping shm_mem_;
  std::array<wl::WlPtr<WlBufferHandler>, kNumBufs> bufs_;
  int next_buf_ = 0;
  uint32_t phase_ = 0;

  // State flags
  bool running_ = true;
  bool configured_ = false;  // set by OnXdgSurfaceConfigure

  // agl_shell binding state (v2+ protocol requires waiting for bound_ok/fail).
  enum class BoundState { Waiting, Ok, Fail };
  BoundState bound_state_ = BoundState::Waiting;

  // Registry recorded names/versions
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t output_name_ = 0, output_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t agl_shell_name_ = 0, agl_shell_ver_ = 0;

  static constexpr int kRoundtripTimeoutMs = 5000;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool SetupShell();
  bool CreateBuffers();
  bool InitialCommit();
  [[nodiscard]] bool MainLoop() const;

  void RequestFrameCallback() noexcept;
  void CommitFrame() noexcept;

  [[nodiscard]] int NextFreeBuf() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler callbacks (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(const uint32_t time_ms) {
  app_->OnFrameDone(time_ms);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  seat_.Release();
}

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) noexcept {
  // AckConfigure is already sent by XdgSurfaceHandler before this is called.
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t width,
                              const int32_t height) noexcept {
  // AGL compositor sends the screen dimensions via xdg_toplevel::configure.
  // Guard against compositor-supplied values that would overflow the SHM
  // allocation arithmetic (same ceiling as ivi-shell/main.cpp).
  // stride=w*4, per_buf=stride*h, total=per_buf*kNumBufs must all fit in a
  // size_t without overflow.  8192×8192×4×2 = 536 MB, well within limits.
  static constexpr int32_t kMaxDim = 8192;
  if (width > 0 && height > 0) {
    if (width > kMaxDim || height > kMaxDim) {
      std::fprintf(stderr,
                   "agl-compositor: toplevel configure %dx%d exceeds maximum "
                   "%d — ignoring\n",
                   width, height, kMaxDim);
      return;
    }
    width_ = width;
    height_ = height;
    std::printf("agl-compositor: toplevel configure %dx%d\n", width, height);
  }
}

void App::OnToplevelClose() noexcept {
  running_ = false;
}

void App::OnAglBoundOk() noexcept {
  bound_state_ = BoundState::Ok;
  std::printf("agl-compositor: bound_ok — shell client accepted\n");
}

void App::OnAglBoundFail() noexcept {
  bound_state_ = BoundState::Fail;
  std::fprintf(
      stderr,
      "agl-compositor: bound_fail — another AGL shell client is already "
      "active\n");
}

void App::OnAglAppState(const char* app_id, const uint32_t state) {
  std::printf("agl-compositor: app_state app_id=%s state=%u\n", app_id, state);
}

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  // SetupShell must come before CreateBuffers so xdg_toplevel::configure
  // can update width_/height_ before the SHM allocation.
  if (!SetupShell())
    return EXIT_FAILURE;
  if (!CreateBuffers())
    return EXIT_FAILURE;
  if (!InitialCommit())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "agl-compositor: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "agl-compositor: wl_display_get_registry failed\n");
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
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == wl_output_traits::interface_name && !output_name_) {
      // Bind the first output advertised.
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
    std::fprintf(stderr, "agl-compositor: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_) {
    std::fprintf(stderr, "agl-compositor: wl_compositor not advertised\n");
    return false;
  }
  if (!shm_name_) {
    std::fprintf(stderr, "agl-compositor: wl_shm not advertised\n");
    return false;
  }
  if (!xdg_wm_base_name_) {
    std::fprintf(stderr,
                 "agl-compositor: xdg_wm_base not advertised "
                 "(compositor missing xdg-shell support?)\n");
    return false;
  }
  if (!agl_shell_name_) {
    std::fprintf(stderr,
                 "agl-compositor: agl_shell not advertised "
                 "(not an AGL compositor?)\n");
    return false;
  }
  if (!output_name_) {
    std::fprintf(stderr, "agl-compositor: no wl_output advertised\n");
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

  // wl_compositor — no events; use Attach() to skip listener installation.
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "agl-compositor: wl_compositor bind failed\n");
    return false;
  }

  // wl_shm
  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "agl-compositor: wl_shm bind failed\n");
    return false;
  }

  // wl_output — no events we care about; use Attach().
  if (wl_proxy* raw = registry_.Bind<wl_output_traits>(
          output_name_, std::min(output_ver_, wl_output_traits::version))) {
    output_.Attach(raw);
  } else {
    std::fprintf(stderr, "agl-compositor: wl_output bind failed\n");
    return false;
  }

  // xdg_wm_base — required for surface role assignment in AGL compositor.
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "agl-compositor: xdg_wm_base bind failed\n");
    return false;
  }

  // agl_shell — bind and install event listener before roundtrip so that
  // bound_ok / bound_fail (since v2) arrive during the roundtrip below.
  if (!wl::BindHandler<agl_shell_traits>(registry_, agl_shell_, agl_shell_name_,
                                         agl_shell_ver_)) {
    std::fprintf(stderr, "agl-compositor: agl_shell bind failed\n");
    return false;
  }
  agl_shell_.Get()->app_ = this;  // needed to dispatch bound_ok/fail

  // wl_seat (optional; provides keyboard for ESC-to-quit)
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "agl-compositor: wl_seat bind failed\n");
    return false;
  }

  // One more roundtrip so wl_shm.format, seat capabilities, and
  // agl_shell.bound_ok / bound_fail (v2+) all arrive.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "agl-compositor: timed out waiting for formats\n");
    return false;
  }

  // Verify agl_shell binding was accepted (required for protocol v2+).
  // A v1 compositor sends no bound event, so Waiting is treated as Ok.
  if (bound_state_ == BoundState::Fail) {
    // bound_fail was received; OnAglBoundFail() already printed an error.
    return false;
  }
  if (bound_state_ == BoundState::Waiting) {
    const uint32_t bound_ver =
        std::min(agl_shell_ver_, agl_shell_traits::version);
    if (bound_ver >= 2u) {
      // Compositor claims v2+ support but sent neither bound_ok nor bound_fail.
      std::fprintf(stderr,
                   "agl-compositor: no bound event received from v%u "
                   "compositor (compositor bug?)\n",
                   bound_ver);
      return false;
    }
    // v1 compositor — no bound events expected; proceed.
    std::printf(
        "agl-compositor: v1 compositor — proceeding without bound "
        "confirmation\n");
  }

  if (!(shm_.Get()->formats & (1u << WL_SHM_FORMAT_XRGB8888))) {
    std::fprintf(stderr,
                 "agl-compositor: XRGB8888 not supported by compositor\n");
    return false;
  }
  return true;
}

// ── SetupShell
// ────────────────────────────────────────────────────────────────
//
// Implements the canonical xdg + agl_shell surface setup sequence used by a
// production AGL shell client.
//
// Critical ordering (crash root-cause if violated):
//  1. wl_surface + xdg_surface + xdg_toplevel creation
//  2. wl_surface.commit() — MUST come first; establishes the committed surface
//     state (including xdg role) that the compositor inspects on set_background
//  3. agl_shell.set_background(surface, output) — AFTER commit so the
//     compositor sees a valid, role-assigned surface
//  4. Dispatch until xdg_surface::configure → ack_configure
//  5. agl_shell.ready() — signal compositor the shell client is initialized

bool App::SetupShell() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // 1. Create the wl_surface that will be used as the background.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "agl-compositor: wl_compositor.create_surface failed\n");
    return false;
  }

  // 2a. Wrap the wl_surface in an xdg_surface.
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr,
                 "agl-compositor: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // 2b. Promote the xdg_surface to a toplevel.
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "agl-compositor: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;
  xdg_toplevel_.Get()->SetTitle("agl-compositor-bg");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.agl-compositor");

  // 3. Empty commit — establishes the xdg_surface role in the committed state.
  //    This MUST come before set_background; the compositor inspects the
  //    committed surface state when it processes set_background.  Sending
  //    set_background before commit leaves the surface with no committed
  //    role and crashes agl-compositor.
  surface_.Get()->Commit();

  // 4. Register this surface as the background for the first output.
  //    Called AFTER the commit, exactly as a production AGL shell client does.
  agl_shell_.Get()->SetBackground(surface_.Get()->GetProxy(),
                                  output_.Get()->GetProxy());
  std::printf("agl-compositor: background surface registered with agl_shell\n");

  // 5. Dispatch until xdg_surface::configure arrives (ack'd automatically by
  //    XdgSurfaceHandler, which then calls OnXdgSurfaceConfigure → configured_
  //    = true).
  while (!configured_) {
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      std::fprintf(stderr,
                   "agl-compositor: timed out waiting for xdg_surface "
                   "configure\n");
      return false;
    }
  }
  std::printf("agl-compositor: xdg_surface configured (%dx%d)\n", width_,
              height_);

  // 6. Signal the compositor that the shell client is fully initialized.
  //    Called after configure is acknowledged, matching the typical AGL shell
  //    flow where the ready signal is sent after all windows finish their
  //    `configure` wait.
  agl_shell_.Get()->Ready();
  std::printf("agl-compositor: agl_shell.ready sent\n");

  return true;
}

// ── CreateBuffers
// ───────────────────────────────────────────────────────────── Called after
// SetupShell() so that width_/height_ reflect any compositor provided
// dimensions from xdg_toplevel::configure.

bool App::CreateBuffers() {
  const std::size_t stride = static_cast<std::size_t>(width_) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(height_);
  const std::size_t total = per_buf * static_cast<std::size_t>(kNumBufs);

  if (!shm_mem_.Create(total)) {
    std::fprintf(stderr, "agl-compositor: SHM allocation failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  // Create the pool using the raw C API (wl_shm has no event we need to
  // handle on the pool itself).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* raw_shm = reinterpret_cast<wl_shm*>(shm_.Get()->GetProxy());
  wl_shm_pool* raw_pool =
      wl_shm_create_pool(raw_shm, shm_mem_.fd, static_cast<int>(total));
  if (!raw_pool) {
    std::fprintf(stderr, "agl-compositor: wl_shm_create_pool failed\n");
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  wl::WlPtr<WlShmPoolHandler> pool;
  pool.Attach(reinterpret_cast<wl_proxy*>(raw_pool));

  for (int i = 0; i < kNumBufs; ++i) {
    const auto offset =
        static_cast<int32_t>(static_cast<std::size_t>(i) * per_buf);
    using wl_buf = wayland::client::wl_buffer_traits;
    using wl_pool = wayland::client::wl_shm_pool_traits;
    if (wl_proxy* raw = wl::construct<wl_buf, wl_pool::Op::CreateBuffer>(
            *pool.Get(), offset, width_, height_, static_cast<int32_t>(stride),
            WL_SHM_FORMAT_XRGB8888)) {
      bufs_.at(static_cast<std::size_t>(i)).Get()->_SetProxy(raw);
    } else {
      std::fprintf(stderr,
                   "agl-compositor: wl_shm_pool.create_buffer[%d] failed\n", i);
      return false;
    }
  }
  pool.Reset();
  return true;
}

// ── NextFreeBuf
// ────────────────────────────────────────────────────────────────

int App::NextFreeBuf() noexcept {
  for (int attempt = 0; attempt < kNumBufs; ++attempt) {
    if (const int idx = (next_buf_ + attempt) % kNumBufs;
        !bufs_.at(static_cast<std::size_t>(idx)).Get()->busy) {
      next_buf_ = (idx + 1) % kNumBufs;
      return idx;
    }
  }
  return -1;
}

// ── InitialCommit
// ─────────────────────────────────────────────────────────────

bool App::InitialCommit() {
  const int idx = NextFreeBuf();
  if (idx < 0) {
    std::fprintf(stderr, "agl-compositor: no free buffer\n");
    return false;
  }

  const std::size_t npixels =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  const std::size_t byte_offset =
      static_cast<std::size_t>(idx) * npixels * sizeof(uint32_t);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto* base = reinterpret_cast<uint32_t*>(
      static_cast<uint8_t*>(shm_mem_.data) + byte_offset);

  paint_pixels({base, npixels}, width_, height_, phase_);

  auto* buf_handler = bufs_.at(static_cast<std::size_t>(idx)).Get();
  buf_handler->busy = true;

  RequestFrameCallback();
  auto* surface = surface_.Get();
  surface->Attach(buf_handler->GetProxy(), 0, 0);
  surface->Damage(0, 0, width_, height_);
  surface->Commit();
  wl_display_flush(display_.Get());
  return true;
}

// ── RequestFrameCallback ─────────────────────────────────────────────────────

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_callback_.Get()->app_ = this;
    frame_callback_.Get()->_SetProxy(raw);
  }
}

// ── CommitFrame
// ───────────────────────────────────────────────────────────────

void App::CommitFrame() noexcept {
  const int idx = NextFreeBuf();
  if (idx < 0)
    return;  // all buffers busy; skip frame

  const std::size_t npixels =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  const std::size_t byte_offset =
      static_cast<std::size_t>(idx) * npixels * sizeof(uint32_t);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto* base = reinterpret_cast<uint32_t*>(
      static_cast<uint8_t*>(shm_mem_.data) + byte_offset);

  paint_pixels({base, npixels}, width_, height_, phase_);
  phase_ += 16'667;  // ~1/60 s in microseconds

  auto* buf_handler = bufs_.at(static_cast<std::size_t>(idx)).Get();
  buf_handler->busy = true;

  RequestFrameCallback();
  auto* surface = surface_.Get();
  surface->Attach(buf_handler->GetProxy(), 0, 0);
  surface->Damage(0, 0, width_, height_);
  surface->Commit();
}

// ── App callbacks
// ─────────────────────────────────────────────────────────────

void App::OnFrameDone(uint32_t /*time_ms*/) noexcept {
  wl_proxy* const spent_cb = frame_callback_.Detach();
  const auto guard = wl::ScopeExit{[spent_cb] {
    if (spent_cb)
      wl_proxy_destroy(spent_cb);
  }};
  CommitFrame();
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.key == KEY_ESC && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

bool App::MainLoop() const {
  std::printf(
      "agl-compositor: running as background shell client "
      "(ESC to quit)\n");
  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "agl-compositor");
  if (ok)
    std::printf("agl-compositor: exiting cleanly\n");
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
