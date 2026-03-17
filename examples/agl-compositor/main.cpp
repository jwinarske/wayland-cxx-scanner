// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// agl-compositor — C++23 AGL shell background client example
//
// Demonstrates the agl_shell protocol: connects to an AGL compositor,
// creates a wl_surface backed by a shared-memory buffer, promotes it to the
// output background with agl_shell.set_background, then calls agl_shell.ready
// to signal the compositor that the shell client is initialised.
//
// The surface draws an animated colour-cycling pattern at ~60 fps.  The
// example runs until SIGINT/SIGTERM or the compositor disconnects.
//
// Usage:
//   agl_compositor [--output N]   (select output index, default 0)
//
// Protocol dependency:
//   agl-shell.xml must be available; the generated agl_shell_client.hpp header
//   provides the agl_shell::client namespace.

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "agl_shell_client.hpp"   // namespace agl_shell::client
#include "wayland_client.hpp"     // namespace wayland::client

// ── System Wayland / Linux C headers ─────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
}

// ── Framework headers ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>

// ── Standard library ──────────────────────────────────────────────────────────
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

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() — core Wayland interfaces
//
// wl_seat_traits::wl_iface() and wl_keyboard_traits::wl_iface() are provided
// inline by <wl/seat.hpp>.
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
// agl_shell wl_interface definitions
//
// The agl_shell protocol is not part of wayland-protocols and has no
// pre-built system wl_interface symbols, so we define them here.
// ══════════════════════════════════════════════════════════════════════════════

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* agl_shell_types[] = {
    nullptr,               // [0] scalar / null
    &wl_surface_interface, // [1] wl_surface 'o' arg
    &wl_output_interface,  // [2] wl_output  'o' arg
    nullptr,               // [3] scalar: edge 'u' in set_panel
    nullptr,               // [4] scalar: app_id 's' in activate_app
    &wl_output_interface,  // [5] wl_output 'o' arg in activate_app
    nullptr,               // [6] scalar: app_id 's' in app_state
    nullptr,               // [7] scalar: state  'u' in app_state
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// clang-format off
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
static constexpr wl_message agl_shell_requests[] = {
    {"ready",          "",    nullptr},
    {"set_background", "oo",  &agl_shell_types[1]},
    {"set_panel",      "oou", &agl_shell_types[1]},
    {"activate_app",   "so",  &agl_shell_types[4]},
};
static constexpr wl_message agl_shell_events[] = {
    {"app_state", "su", &agl_shell_types[6]},
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)
// clang-format on

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static wl_interface agl_shell_iface_def = {
    "agl_shell", 1,
    4, std::data(agl_shell_requests),
    1, std::data(agl_shell_events)};

namespace agl_shell::client {
const wl_interface& agl_shell_traits::wl_iface() noexcept {
  return agl_shell_iface_def;
}
}  // namespace agl_shell::client

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

  [[nodiscard]] bool Create(std::size_t n) noexcept {
    fd = memfd_create("agl-compositor-bg", 0);
    if (fd < 0)
      return false;
    if (ftruncate(fd, static_cast<off_t>(n)) < 0)
      return false;
    data = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
      return false;
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

/// Fill @p image (XRGB8888) with an animated colour wheel.
static void paint_pixels(void* image,
                         int width,
                         int height,
                         uint32_t phase) noexcept {
  const int halfh = height / 2;
  const int halfw = width / 2;
  auto* base = static_cast<uint32_t*>(image);

  const double ang =
      M_PI * 2.0 / 1'000'000.0 * static_cast<double>(phase);
  const double s = std::sin(ang);
  const double c = std::cos(ang);

  const int outer_r_sq = [&] {
    int r = (halfw < halfh ? halfw : halfh) - 16;
    return r * r;
  }();

  for (int y = 0; y < height; ++y) {
    const int oy = y - halfh;
    const int y2 = oy * oy;
    for (int x = 0; x < width; ++x) {
      const int ox = x - halfw;
      const int idx = y * width + x;
      if (ox * ox + y2 > outer_r_sq) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        base[idx] = (ox * oy > 0) ? 0xFF000000u : 0xFFFFFFFFu;
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
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      base[idx] = v;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

class App;

// ── WlCompositorHandler ───────────────────────────────────────────────────────

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlShmPoolHandler ──────────────────────────────────────────────────────────

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlShmHandler ──────────────────────────────────────────────────────────────

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  uint32_t formats = 0;
  void OnFormat(uint32_t fmt) override {
    if (fmt < 32u)
      formats |= (1u << fmt);
  }
};

// ── WlBufferHandler ───────────────────────────────────────────────────────────

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool busy = false;
  void OnRelease() override { busy = false; }
};

// ── WlSurfaceHandler ─────────────────────────────────────────────────────────

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// ── WlOutputHandler ───────────────────────────────────────────────────────────
// Minimal wl_output handler; we only need the proxy for set_background.

class WlOutputHandler : public wayland::client::CWlOutput<WlOutputHandler> {
};

// ── WlCallbackHandler ─────────────────────────────────────────────────────────

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// ── AglShellHandler ───────────────────────────────────────────────────────────

class AglShellHandler
    : public agl_shell::client::CAglShell<AglShellHandler> {
 public:
  void OnAppState(const char* app_id, uint32_t state) override {
    std::printf("agl-compositor: app_state app_id=%s state=%u\n", app_id,
                state);
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks from CRTP handlers ──────────────────────────────────────────
  void OnKey(uint32_t key, uint32_t state);
  void OnFrameDone(uint32_t time_ms) noexcept;

 private:
  // Declaration order = reverse destruction order.
  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  // Core Wayland objects
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler>        shm_;
  wl::WlPtr<WlSurfaceHandler>    surface_;

  // AGL shell
  wl::WlPtr<AglShellHandler> agl_shell_;

  // Output — we bind the first advertised wl_output
  wl::WlPtr<WlOutputHandler> output_;

  // Seat/keyboard (optional; ESC to quit)
  wl::SeatManager<App> seat_;

  // Frame-pacing callback
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // SHM backing store — two buffers for double-buffering
  static constexpr int kWidth  = 1920;
  static constexpr int kHeight = 1080;
  static constexpr int kNumBufs = 2;

  ShmMapping shm_mem_;
  std::array<wl::WlPtr<WlBufferHandler>, kNumBufs> bufs_;
  int next_buf_ = 0;
  uint32_t phase_ = 0;

  // State
  bool running_ = true;
  bool shell_ready_ = false;

  // Registry recorded names/versions
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_       = 0, shm_ver_       = 0;
  uint32_t output_name_     = 0, output_ver_    = 0;
  uint32_t agl_shell_name_  = 0, agl_shell_ver_ = 0;

  static constexpr int kRoundtripTimeoutMs = 5000;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateBuffers();
  bool SetupShell();
  bool InitialCommit();
  [[nodiscard]] bool MainLoop();

  void RequestFrameCallback() noexcept;
  void CommitFrame() noexcept;

  [[nodiscard]] int NextFreeBuf() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler callbacks (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  app_->OnFrameDone(time_ms);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  seat_.Release();
}

int App::Run() {
  if (!ConnectDisplay())  return EXIT_FAILURE;
  if (!ScanGlobals())     return EXIT_FAILURE;
  if (!BindGlobals())     return EXIT_FAILURE;
  if (!CreateBuffers())   return EXIT_FAILURE;
  if (!SetupShell())      return EXIT_FAILURE;
  if (!InitialCommit())   return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "agl-compositor: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "agl-compositor: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                             std::string_view iface, uint32_t ver) {
    using namespace wayland::client;
    using namespace agl_shell::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name; compositor_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name; shm_ver_ = ver;
    } else if (iface == wl_output_traits::interface_name && !output_name_) {
      // Bind the first output advertised.
      output_name_ = name; output_ver_ = ver;
    } else if (iface == agl_shell_traits::interface_name) {
      agl_shell_name_ = name; agl_shell_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "agl-compositor: timed out waiting for globals\n");
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

// ── BindGlobals ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
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
  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_,
                                       shm_ver_)) {
    std::fprintf(stderr, "agl-compositor: wl_shm bind failed\n");
    return false;
  }

  // wl_output — no events we care about; use Attach().
  if (wl_proxy* raw = registry_.Bind<wl_output_traits>(
          output_name_,
          std::min(output_ver_, wl_output_traits::version))) {
    output_.Attach(raw);
  } else {
    std::fprintf(stderr, "agl-compositor: wl_output bind failed\n");
    return false;
  }

  // agl_shell
  if (!wl::BindHandler<agl_shell_traits>(registry_, agl_shell_,
                                          agl_shell_name_,
                                          agl_shell_ver_)) {
    std::fprintf(stderr, "agl-compositor: agl_shell bind failed\n");
    return false;
  }

  // wl_seat (optional; provides keyboard for ESC-to-quit)
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "agl-compositor: wl_seat bind failed\n");
    return false;
  }

  // One more roundtrip so wl_shm.format and seat capabilities arrive.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "agl-compositor: timed out waiting for formats\n");
    return false;
  }

  if (!(shm_.Get()->formats & (1u << WL_SHM_FORMAT_XRGB8888))) {
    std::fprintf(stderr,
                 "agl-compositor: XRGB8888 not supported by compositor\n");
    return false;
  }
  return true;
}

// ── CreateBuffers ─────────────────────────────────────────────────────────────

bool App::CreateBuffers() {
  const std::size_t stride  = static_cast<std::size_t>(kWidth) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(kHeight);
  const std::size_t total   = per_buf * static_cast<std::size_t>(kNumBufs);

  if (!shm_mem_.Create(total)) {
    std::fprintf(stderr, "agl-compositor: SHM allocation failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  // Create the pool using the raw C API (wl_shm has no event we need to
  // handle on the pool itself).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  wl_shm* raw_shm = reinterpret_cast<wl_shm*>(shm_.Get()->GetProxy());
  wl_shm_pool* raw_pool = wl_shm_create_pool(raw_shm, shm_mem_.fd,
                                              static_cast<int>(total));
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
    using wl_buf   = wayland::client::wl_buffer_traits;
    using wl_pool  = wayland::client::wl_shm_pool_traits;
    if (wl_proxy* raw = wl::construct<wl_buf, wl_pool::Op::CreateBuffer>(
            *pool.Get(), offset, kWidth, kHeight,
            static_cast<int32_t>(stride), WL_SHM_FORMAT_XRGB8888)) {
      bufs_.at(static_cast<std::size_t>(i)).Get()->_SetProxy(raw);
    } else {
      std::fprintf(stderr,
                   "agl-compositor: wl_shm_pool.create_buffer[%d] failed\n",
                   i);
      return false;
    }
  }
  pool.Reset();
  return true;
}

// ── SetupShell ────────────────────────────────────────────────────────────────

bool App::SetupShell() {
  using namespace wayland::client;

  // Create wl_surface.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "agl-compositor: wl_compositor.create_surface failed\n");
    return false;
  }

  // Register the surface as the background for the first output.
  agl_shell_.Get()->SetBackground(surface_.Get()->GetProxy(),
                                   output_.Get()->GetProxy());
  std::printf("agl-compositor: background surface registered\n");

  // Signal the compositor that the shell client is ready.
  agl_shell_.Get()->Ready();
  shell_ready_ = true;
  std::printf("agl-compositor: agl_shell.ready sent\n");
  return true;
}

// ── CreateBuffers helper ───────────────────────────────────────────────────────

int App::NextFreeBuf() noexcept {
  for (int attempt = 0; attempt < kNumBufs; ++attempt) {
    const int idx = (next_buf_ + attempt) % kNumBufs;
    if (!bufs_.at(static_cast<std::size_t>(idx)).Get()->busy) {
      next_buf_ = (idx + 1) % kNumBufs;
      return idx;
    }
  }
  return -1;
}

// ── InitialCommit ─────────────────────────────────────────────────────────────

bool App::InitialCommit() {
  const int idx = NextFreeBuf();
  if (idx < 0) {
    std::fprintf(stderr, "agl-compositor: no free buffer\n");
    return false;
  }

  const std::size_t stride = static_cast<std::size_t>(kWidth) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(kHeight);
  void* pixels =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      static_cast<uint8_t*>(shm_mem_.data) +
      static_cast<std::size_t>(idx) * per_buf;

  paint_pixels(pixels, kWidth, kHeight, phase_);

  auto* buf_handler = bufs_.at(static_cast<std::size_t>(idx)).Get();
  buf_handler->busy = true;

  RequestFrameCallback();
  surface_.Get()->Attach(buf_handler->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, kWidth, kHeight);
  surface_.Get()->Commit();
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

// ── CommitFrame ───────────────────────────────────────────────────────────────

void App::CommitFrame() noexcept {
  const int idx = NextFreeBuf();
  if (idx < 0)
    return;  // all buffers busy; skip frame

  const std::size_t stride  = static_cast<std::size_t>(kWidth) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(kHeight);
  void* pixels =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      static_cast<uint8_t*>(shm_mem_.data) +
      static_cast<std::size_t>(idx) * per_buf;

  paint_pixels(pixels, kWidth, kHeight, phase_);
  phase_ += 16'667;  // ~1/60 s in microseconds

  auto* buf_handler = bufs_.at(static_cast<std::size_t>(idx)).Get();
  buf_handler->busy = true;

  RequestFrameCallback();
  surface_.Get()->Attach(buf_handler->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, kWidth, kHeight);
  surface_.Get()->Commit();
}

// ── App callbacks ─────────────────────────────────────────────────────────────

void App::OnFrameDone(uint32_t /*time_ms*/) noexcept {
  wl_proxy* const spent_cb = frame_callback_.Detach();
  const auto guard = wl::ScopeExit{[spent_cb] {
    if (spent_cb)
      wl_proxy_destroy(spent_cb);
  }};
  CommitFrame();
}

void App::OnKey(const uint32_t key, const uint32_t state) {
  if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

// ── MainLoop ──────────────────────────────────────────────────────────────────

bool App::MainLoop() {
  std::printf(
      "agl-compositor: running as background shell client "
      "(ESC to quit)\n");
  const bool ok =
      wl::RunEventLoop(display_.Get(), [this] { return !running_; },
                       "agl-compositor");
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
