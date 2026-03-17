// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// ivi-shell — C++23 IVI-application client example
//
// Demonstrates the ivi_application protocol: connects to a compositor that
// implements IVI-style surface management (e.g. Weston with the IVI shell
// plugin), creates a wl_surface backed by a shared-memory buffer, and
// registers it with an integer IVI surface ID via
// ivi_application.surface_create.
//
// The surface draws an animated colour-cycling pattern and responds to
// ivi_surface.configure events (compositor-driven resize hints).
//
// Usage:
//   ivi_shell [IVI_ID]    (IVI surface ID, default 9000)
//
// Protocol dependency:
//   ivi-application.xml (bundled in protocols/) provides the protocol;
//   no system package is required beyond wayland-scanner.

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "ivi_application_client.hpp"  // namespace ivi_application::client
#include "wayland_client.hpp"          // namespace wayland::client

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

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// ivi_application / ivi_surface wl_interface definitions
//
// These interfaces are not part of the core Wayland library, so we define
// the wl_interface structures manually here (the same technique used by
// presentation-shm for xdg-shell).
// ══════════════════════════════════════════════════════════════════════════════

extern const wl_interface ivi_surface_iface_def;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* ivi_application_types[] = {
    nullptr,               // [0] scalar ('i' args in configure; 'u' in surface_create)
    &wl_surface_interface, // [1] 'o' wl_surface in surface_create
    &ivi_surface_iface_def,// [2] 'n' ivi_surface in surface_create
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
static constexpr wl_message ivi_surface_requests[] = {
    {"destroy", "", nullptr},
};
static constexpr wl_message ivi_surface_events[] = {
    {"configure", "ii", &ivi_application_types[0]},
};
static constexpr wl_message ivi_application_requests[] = {
    // surface_create(ivi_id: u, surface: o, id: n) — format "uon"
    {"surface_create", "uon", &ivi_application_types[0]},
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)

// clang-format off
const wl_interface ivi_surface_iface_def = {
    "ivi_surface",    1,
    1, std::data(ivi_surface_requests), 1, std::data(ivi_surface_events)};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static wl_interface ivi_application_iface_def = {
    "ivi_application", 1,
    1, std::data(ivi_application_requests), 0, nullptr};
// clang-format on

namespace ivi_application::client {
const wl_interface& ivi_surface_traits::wl_iface() noexcept {
  return ivi_surface_iface_def;
}
const wl_interface& ivi_application_traits::wl_iface() noexcept {
  return ivi_application_iface_def;
}
}  // namespace ivi_application::client

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
    fd = memfd_create("ivi-shell", 0);
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

// ── WlCallbackHandler ─────────────────────────────────────────────────────────

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// ── IviApplicationHandler ────────────────────────────────────────────────────
// ivi_application has no events; provide the required ProcessEvent stub.

class IviApplicationHandler
    : public ivi_application::client::CIviApplication<IviApplicationHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── IviSurfaceHandler ─────────────────────────────────────────────────────────

class IviSurfaceHandler
    : public ivi_application::client::CIviSurface<IviSurfaceHandler> {
 public:
  App* app_ = nullptr;
  void OnConfigure(int32_t width, int32_t height) override;
};

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  explicit App(uint32_t ivi_id) : ivi_id_(ivi_id) {}
  int Run();
  ~App();

  // ── Callbacks from CRTP handlers ──────────────────────────────────────────
  void OnKey(uint32_t key, uint32_t state);
  void OnFrameDone(uint32_t time_ms) noexcept;
  void OnIviConfigure(int32_t width, int32_t height) noexcept;

 private:
  // IVI surface ID (from command line or default)
  uint32_t ivi_id_;

  // Declaration order = reverse destruction order.
  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  // Core Wayland objects
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler>        shm_;
  wl::WlPtr<WlSurfaceHandler>    surface_;

  // IVI-application objects
  wl::WlPtr<IviApplicationHandler> ivi_app_;
  wl::WlPtr<IviSurfaceHandler>     ivi_surface_;

  // Seat/keyboard (optional; ESC to quit)
  wl::SeatManager<App> seat_;

  // Frame-pacing callback
  wl::WlPtr<WlCallbackHandler> frame_callback_;

  // SHM backing store — double-buffered
  static constexpr int kInitWidth  = 800;
  static constexpr int kInitHeight = 600;
  static constexpr int kNumBufs    = 2;

  int width_  = kInitWidth;
  int height_ = kInitHeight;

  ShmMapping shm_mem_;
  std::array<wl::WlPtr<WlBufferHandler>, kNumBufs> bufs_;
  int next_buf_ = 0;
  uint32_t phase_ = 0;

  // State
  bool running_    = true;
  bool configured_ = false;

  // Registry recorded names/versions
  uint32_t compositor_name_  = 0, compositor_ver_  = 0;
  uint32_t shm_name_         = 0, shm_ver_         = 0;
  uint32_t ivi_app_name_     = 0, ivi_app_ver_     = 0;

  static constexpr int kRoundtripTimeoutMs = 5000;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateBuffers();
  bool CreateIviSurface();
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

void IviSurfaceHandler::OnConfigure(int32_t width, int32_t height) {
  app_->OnIviConfigure(width, height);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  seat_.Release();
}

int App::Run() {
  if (!ConnectDisplay())   return EXIT_FAILURE;
  if (!ScanGlobals())      return EXIT_FAILURE;
  if (!BindGlobals())      return EXIT_FAILURE;
  if (!CreateBuffers())    return EXIT_FAILURE;
  if (!CreateIviSurface()) return EXIT_FAILURE;
  if (!InitialCommit())    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "ivi-shell: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "ivi-shell: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                             std::string_view iface, uint32_t ver) {
    using namespace wayland::client;
    using namespace ivi_application::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name; compositor_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name; shm_ver_ = ver;
    } else if (iface == ivi_application_traits::interface_name) {
      ivi_app_name_ = name; ivi_app_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "ivi-shell: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_) {
    std::fprintf(stderr, "ivi-shell: wl_compositor not advertised\n");
    return false;
  }
  if (!shm_name_) {
    std::fprintf(stderr, "ivi-shell: wl_shm not advertised\n");
    return false;
  }
  if (!ivi_app_name_) {
    std::fprintf(stderr,
                 "ivi-shell: ivi_application not advertised "
                 "(IVI compositor not running?)\n");
    return false;
  }
  return true;
}

// ── BindGlobals ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace ivi_application::client;

  // wl_compositor — no events; Attach() skips listener installation.
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "ivi-shell: wl_compositor bind failed\n");
    return false;
  }

  // wl_shm
  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_,
                                       shm_ver_)) {
    std::fprintf(stderr, "ivi-shell: wl_shm bind failed\n");
    return false;
  }

  // ivi_application — no events; Attach() skips listener installation.
  if (wl_proxy* raw = registry_.Bind<ivi_application_traits>(
          ivi_app_name_,
          std::min(ivi_app_ver_, ivi_application_traits::version))) {
    ivi_app_.Attach(raw);
  } else {
    std::fprintf(stderr, "ivi-shell: ivi_application bind failed\n");
    return false;
  }

  // wl_seat (optional; enables keyboard handling for ESC-to-quit)
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "ivi-shell: wl_seat bind failed\n");
    return false;
  }

  // Roundtrip so wl_shm.format events arrive.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "ivi-shell: timed out waiting for formats\n");
    return false;
  }

  if (!(shm_.Get()->formats & (1u << WL_SHM_FORMAT_XRGB8888))) {
    std::fprintf(stderr,
                 "ivi-shell: XRGB8888 not supported by compositor\n");
    return false;
  }
  return true;
}

// ── CreateBuffers ─────────────────────────────────────────────────────────────

bool App::CreateBuffers() {
  const std::size_t stride  = static_cast<std::size_t>(width_) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(height_);
  const std::size_t total   = per_buf * static_cast<std::size_t>(kNumBufs);

  if (!shm_mem_.Create(total)) {
    std::fprintf(stderr, "ivi-shell: SHM allocation failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  wl_shm* raw_shm = reinterpret_cast<wl_shm*>(shm_.Get()->GetProxy());
  wl_shm_pool* raw_pool = wl_shm_create_pool(raw_shm, shm_mem_.fd,
                                              static_cast<int>(total));
  if (!raw_pool) {
    std::fprintf(stderr, "ivi-shell: wl_shm_create_pool failed\n");
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  wl::WlPtr<WlShmPoolHandler> pool;
  pool.Attach(reinterpret_cast<wl_proxy*>(raw_pool));

  for (int i = 0; i < kNumBufs; ++i) {
    const auto offset =
        static_cast<int32_t>(static_cast<std::size_t>(i) * per_buf);
    using wl_buf  = wayland::client::wl_buffer_traits;
    using wl_pool = wayland::client::wl_shm_pool_traits;
    if (wl_proxy* raw = wl::construct<wl_buf, wl_pool::Op::CreateBuffer>(
            *pool.Get(), offset, width_, height_,
            static_cast<int32_t>(stride), WL_SHM_FORMAT_XRGB8888)) {
      bufs_.at(static_cast<std::size_t>(i)).Get()->_SetProxy(raw);
    } else {
      std::fprintf(stderr,
                   "ivi-shell: wl_shm_pool.create_buffer[%d] failed\n", i);
      return false;
    }
  }
  pool.Reset();
  return true;
}

// ── CreateIviSurface ──────────────────────────────────────────────────────────

bool App::CreateIviSurface() {
  using namespace wayland::client;
  using namespace ivi_application::client;

  // Create the wl_surface.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "ivi-shell: wl_compositor.create_surface failed\n");
    return false;
  }

  // Create the ivi_surface by calling ivi_application.surface_create.
  //
  // The request format is "uon" (uint ivi_id, object wl_surface, new_id).
  // wl::construct<> places nullptr (new_id placeholder) BEFORE extra args,
  // which only works for "n" or "no" signatures.  For "uon" we call
  // _MarshalNew() directly, passing the wire args in protocol order with
  // nullptr for the new_id placeholder at the end.
  wl_proxy* ivi_surf_raw = ivi_app_.Get()->_MarshalNew(
      ivi_application_traits::Op::SurfaceCreate,
      &ivi_surface_traits::wl_iface(),
      ivi_id_,                      // uint32_t ivi_id
      surface_.Get()->GetProxy(),   // wl_proxy* wl_surface
      nullptr                       // new_id placeholder
  );
  if (!ivi_surf_raw) {
    std::fprintf(stderr,
                 "ivi-shell: ivi_application.surface_create failed "
                 "(IVI ID %u already in use?)\n",
                 ivi_id_);
    return false;
  }
  ivi_surface_.Get()->_SetProxy(ivi_surf_raw);
  ivi_surface_.Get()->app_ = this;
  std::printf("ivi-shell: IVI surface created (id=%u)\n", ivi_id_);

  // Commit the surface so the compositor can send a configure event.
  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "ivi-shell: timed out waiting for ivi_surface configure\n");
    return false;
  }
  return true;
}

// ── Buffer helpers ────────────────────────────────────────────────────────────

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
    std::fprintf(stderr, "ivi-shell: no free buffer for initial commit\n");
    return false;
  }

  const std::size_t stride  = static_cast<std::size_t>(width_) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(height_);
  void* pixels =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      static_cast<uint8_t*>(shm_mem_.data) +
      static_cast<std::size_t>(idx) * per_buf;

  paint_pixels(pixels, width_, height_, phase_);

  auto* buf = bufs_.at(static_cast<std::size_t>(idx)).Get();
  buf->busy = true;

  RequestFrameCallback();
  surface_.Get()->Attach(buf->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, width_, height_);
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

  const std::size_t stride  = static_cast<std::size_t>(width_) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(height_);
  void* pixels =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      static_cast<uint8_t*>(shm_mem_.data) +
      static_cast<std::size_t>(idx) * per_buf;

  paint_pixels(pixels, width_, height_, phase_);
  phase_ += 16'667;  // ~1/60 s in microseconds

  auto* buf = bufs_.at(static_cast<std::size_t>(idx)).Get();
  buf->busy = true;

  RequestFrameCallback();
  surface_.Get()->Attach(buf->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, width_, height_);
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

void App::OnIviConfigure(int32_t width, int32_t height) noexcept {
  if (width <= 0 || height <= 0)
    return;

  // Guard against dimensions that would overflow int32_t in pool/buffer sizes.
  // stride=w*4, per_buf=stride*h, total=per_buf*2 must all fit in int32_t.
  // At 16384×16384: total = 16384*4*16384*2 = 2 GB exactly — on the edge.
  // Use 8192 as a safe ceiling: total ≤ 8192*4*8192*2 = 536 MB < INT32_MAX.
  static constexpr int32_t kMaxDim = 8192;
  if (width > kMaxDim || height > kMaxDim) {
    std::fprintf(stderr,
                 "ivi-shell: configure %d×%d exceeds maximum %d — ignoring\n",
                 width, height, kMaxDim);
    return;
  }

  std::printf("ivi-shell: configure %d×%d\n", width, height);
  if (width == width_ && height == height_)
    return;

  // Resize: recreate SHM buffers at the new dimensions.
  width_  = width;
  height_ = height;

  // Destroy the buffer proxies (sends wl_buffer.destroy for each).  Then do a
  // roundtrip so the compositor can process the destroys and flush any pending
  // wl_buffer.release events before we munmap the underlying SHM region.
  for (auto& b : bufs_)
    b.Reset();
  // Critical: ensures the compositor has finished reading the SHM region
  // (signalled by wl_buffer.release) before munmap in shm_mem_.Reset() below.
  wl_display_roundtrip(display_.Get());
  shm_mem_.Reset();
  next_buf_ = 0;

  const std::size_t stride  = static_cast<std::size_t>(width_) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(height_);
  const std::size_t total   = per_buf * static_cast<std::size_t>(kNumBufs);

  if (!shm_mem_.Create(total)) {
    std::fprintf(stderr, "ivi-shell: SHM resize failed: %s\n",
                 std::strerror(errno));
    running_ = false;
    return;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  wl_shm* raw_shm = reinterpret_cast<wl_shm*>(shm_.Get()->GetProxy());
  wl_shm_pool* raw_pool = wl_shm_create_pool(raw_shm, shm_mem_.fd,
                                              static_cast<int>(total));
  if (!raw_pool) {
    std::fprintf(stderr, "ivi-shell: wl_shm_create_pool (resize) failed\n");
    running_ = false;
    return;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  wl::WlPtr<WlShmPoolHandler> pool;
  pool.Attach(reinterpret_cast<wl_proxy*>(raw_pool));

  for (int i = 0; i < kNumBufs; ++i) {
    const auto offset =
        static_cast<int32_t>(static_cast<std::size_t>(i) * per_buf);
    using wl_buf  = wayland::client::wl_buffer_traits;
    using wl_pool = wayland::client::wl_shm_pool_traits;
    if (wl_proxy* raw = wl::construct<wl_buf, wl_pool::Op::CreateBuffer>(
            *pool.Get(), offset, width_, height_,
            static_cast<int32_t>(stride), WL_SHM_FORMAT_XRGB8888)) {
      bufs_.at(static_cast<std::size_t>(i)).Get()->_SetProxy(raw);
    } else {
      std::fprintf(stderr,
                   "ivi-shell: wl_shm_pool.create_buffer[%d] (resize) "
                   "failed\n", i);
      running_ = false;
      return;
    }
  }
  pool.Reset();
  configured_ = true;
}

void App::OnKey(const uint32_t key, const uint32_t state) {
  if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

// ── MainLoop ──────────────────────────────────────────────────────────────────

bool App::MainLoop() {
  std::printf(
      "ivi-shell: running with IVI surface ID %u (ESC to quit)\n",
      ivi_id_);
  const bool ok =
      wl::RunEventLoop(display_.Get(), [this] { return !running_; },
                       "ivi-shell");
  if (ok)
    std::printf("ivi-shell: exiting cleanly\n");
  return ok;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);

  uint32_t ivi_id = 9000u;  // default IVI surface ID
  if (argc >= 2) {
    char* end = nullptr;
    const long val = std::strtol(argv[1], &end, 10);
    // Reject partial parses, overflow, and 0 (IVI ID 0 is reserved/invalid
    // on all known IVI compositors; strtol also returns 0 for non-numeric
    // input, so this check catches both cases).
    if (end != argv[1] && *end == '\0' && val > 0 &&
        val <= static_cast<long>(UINT32_MAX))
      ivi_id = static_cast<uint32_t>(val);
    else
      std::fprintf(stderr,
                   "ivi-shell: invalid IVI ID '%s' — using default %u\n",
                   argv[1], ivi_id);
  }

  std::printf("ivi-shell: using IVI surface ID %u\n", ivi_id);
  App app{ivi_id};
  return app.Run();
}
