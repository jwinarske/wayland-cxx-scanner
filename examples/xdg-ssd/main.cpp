// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// xdg-ssd — Server-Side Decoration example
//
// The complement to the xdg-csd example: rather than drawing its own title
// bar and borders, this client lets the compositor frame the window. When the
// compositor offers zxdg_decoration_manager_v1 it creates a toplevel
// decoration object and requests ZxdgToplevelDecorationV1Mode::ServerSide;
// the client then only ever paints its content area.
//
// ── On Weston specifically ──────────────────────────────────────────────────
// Weston's desktop-shell draws a server-side frame (title bar, close button,
// resize borders) around *every* xdg_toplevel automatically, and — at least
// up to Weston 14 — it does NOT advertise zxdg_decoration_manager_v1. So on
// Weston you get server-side decorations without the protocol ever being
// negotiated. The decoration manager is therefore treated as optional here:
// when present (e.g. Sway/wlroots, KWin) we explicitly request ServerSide and
// report the negotiated mode; when absent we note it and keep running, which
// is exactly what makes the example testable on Weston.
//
// The compositor is still free to answer a ServerSide request with ClientSide
// (GNOME/Mutter forces this); if it does we report the request was declined
// and keep running with an undecorated window.
//
// Test with Weston:
//   weston --width=900 --height=700 &
//   WAYLAND_DISPLAY=wayland-1 ./xdg_ssd
//
// Usage:
//   xdg_ssd [-w WIDTH] [-h HEIGHT] [-t TITLE]

// clang-tidy: suppress diagnostics common to Wayland C-API boundary code.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-pro-bounds-pointer-arithmetic,
//             cppcoreguidelines-pro-type-reinterpret-cast)

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "wayland_client.hpp"                     // namespace wayland::client
#include "xdg_decoration_unstable_v1_client.hpp"  // xdg_decoration_unstable_v1::client
#include "xdg_shell_client.hpp"                   // namespace xdg_shell::client

// ── Framework headers ────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/span.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_decoration.hpp>
#include <wl/xdg_shell.hpp>

// ── System Wayland C headers ─────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-util.h>
}

// ── Standard library ─────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() — core Wayland interfaces
//
// wl_seat_traits / wl_keyboard_traits wl_iface() are provided by <wl/seat.hpp>.
// All xdg_shell traits are provided inline by <wl/xdg_shell.hpp>.
// All xdg_decoration traits are provided inline by <wl/xdg_decoration.hpp>.
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
    fd = memfd_create("xdg-ssd", MFD_CLOEXEC);
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
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

class App;

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
};

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  uint32_t formats = 0;
  void OnFormat(uint32_t fmt) override {
    if (fmt < 32u)
      formats |= (1u << fmt);
  }
};

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool busy = false;
  void OnRelease() override { busy = false; }
};

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// XDG shell + decoration handlers come from the framework:
//   wl::XdgWmBaseHandler         — answers ping automatically
//   wl::XdgSurfaceHandler<App>   — acks configure, calls OnXdgSurfaceConfigure
//   wl::XdgToplevelHandler<App>  — delegates configure/close to App
//   wl::XdgDecorationManagerHandler
//   wl::XdgDecorationHandler<App> — delegates configure to App

// ══════════════════════════════════════════════════════════════════════════════
// Buffer pool — two double-buffered wl_shm buffers
// ══════════════════════════════════════════════════════════════════════════════

static constexpr int kNumBuffers = 2;

// Upper bound on a single window dimension. Bounds the SHM allocation so the
// total pool size (stride * height * kNumBuffers) stays well within INT_MAX —
// wl_shm_create_pool takes an int — and guards against integer overflow from
// untrusted sizes (compositor configure events and -w/-h command-line flags).
static constexpr int kMaxDim = 8192;

struct BufferPool {
  ShmMapping mem;
  std::array<wl::WlPtr<WlBufferHandler>, static_cast<std::size_t>(kNumBuffers)>
      bufs;
  int next = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] bool Create(int w, int h, wl_proxy* shm_raw) noexcept;

  [[nodiscard]] bool Recreate(int w, int h, wl_proxy* shm_raw) noexcept {
    for (auto& b : bufs)
      b.Reset();
    mem.Reset();
    next = 0;
    return Create(w, h, shm_raw);
  }

  [[nodiscard]] void* PixelData(int i) const noexcept {
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    return static_cast<uint8_t*>(mem.data) +
           static_cast<std::size_t>(i) * stride *
               static_cast<std::size_t>(height);
  }

  [[nodiscard]] int NextFree() noexcept {
    for (int attempt = 0; attempt < kNumBuffers; ++attempt) {
      const int idx = (next + attempt) % kNumBuffers;
      if (!bufs.at(static_cast<std::size_t>(idx)).Get()->busy) {
        next = (idx + 1) % kNumBuffers;
        return idx;
      }
    }
    return -1;
  }
};

bool BufferPool::Create(int w, int h, wl_proxy* shm_raw) noexcept {
  using namespace wayland::client;
  width = w;
  height = h;

  // Defense in depth: callers clamp dimensions to kMaxDim, but reject anything
  // that would overflow the int the pool size is passed as.
  if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) {
    std::fprintf(stderr, "xdg-ssd: refusing %dx%d buffer (out of range)\n", w,
                 h);
    return false;
  }

  const std::size_t stride = static_cast<std::size_t>(w) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(h);
  const std::size_t total = per_buf * static_cast<std::size_t>(kNumBuffers);

  if (!mem.Create(total)) {
    std::fprintf(stderr, "xdg-ssd: SHM allocation failed\n");
    return false;
  }

  wl::WlPtr<WlShmPoolHandler> pool;
  {
    wl_shm_pool* raw_pool = wl_shm_create_pool(
        reinterpret_cast<wl_shm*>(shm_raw), mem.fd, static_cast<int>(total));
    if (!raw_pool) {
      std::fprintf(stderr, "xdg-ssd: wl_shm_create_pool failed\n");
      return false;
    }
    pool.Attach(reinterpret_cast<wl_proxy*>(raw_pool));
  }

  for (int i = 0; i < kNumBuffers; ++i) {
    const auto offset =
        static_cast<int32_t>(static_cast<std::size_t>(i) * per_buf);
    if (wl_proxy* raw = wl::construct<wl_buffer_traits,
                                      wl_shm_pool_traits::Op::CreateBuffer>(
            *pool.Get(), offset, w, h, static_cast<int32_t>(stride),
            WL_SHM_FORMAT_XRGB8888)) {
      bufs.at(static_cast<std::size_t>(i)).Get()->_SetProxy(raw);
    } else {
      std::fprintf(stderr, "xdg-ssd: wl_shm_pool.create_buffer [%d] failed\n",
                   i);
      return false;
    }
  }

  pool.Reset();
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Content painting — the whole surface is content; the compositor frames it.
// ══════════════════════════════════════════════════════════════════════════════

static void paint_content(wl::span<uint32_t> buf,
                          int width,
                          int height,
                          uint32_t time) noexcept {
  // A simple animated diagonal gradient so it is obvious the client is the one
  // painting the interior while the compositor owns the surrounding frame.
  const uint32_t t = time / 16u;
  for (int y = 0; y < height; ++y) {
    const auto row = static_cast<uint32_t>(y);
    // Green channel depends only on the row, so compute it once per scanline.
    const uint32_t g = ((row + t) & 0xFFu) << 8;
    uint32_t* dst =
        &buf[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)];
    for (int x = 0; x < width; ++x) {
      const auto col = static_cast<uint32_t>(x);
      const uint32_t r = (col + t) & 0xFFu;
      const uint32_t b = (col + row) & 0xFFu;
      dst[x] = 0xFF000000u | (r << 16) | g | b;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  App(int w, int h, const char* title) : width_(w), height_(h), title_(title) {}
  ~App();

  int Run();

  // ── Callbacks from CRTP handlers ──────────────────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t width, int32_t height);
  void OnToplevelClose();
  void OnDecorationConfigure(uint32_t mode);
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameDone(uint32_t stamp_ms) noexcept;

 private:
  int width_;
  int height_;
  const char* title_;

  // ── Wayland objects ───────────────────────────────────────────────────────
  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;

  wl::WlPtr<wl::XdgDecorationManagerHandler> decoration_mgr_;
  wl::WlPtr<wl::XdgDecorationHandler<App>> decoration_;

  wl::SeatManager<App> seat_;

  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  wl::WlPtr<WlCallbackHandler> frame_cb_;
  BufferPool pool_;

  // ── State ─────────────────────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  bool geometry_dirty_ = true;  // resend window geometry on next frame

  // ── Global IDs from registry scan ─────────────────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t decoration_mgr_name_ = 0, decoration_mgr_ver_ = 0;

  // ── Pipeline ──────────────────────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateWindow();
  bool CreateBuffers();
  bool MainLoop();

  void RequestFrameCallback() noexcept;
  void CommitFrame(uint32_t time_ms) noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler implementations needing the full App definition
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  app_->OnFrameDone(time_ms);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

static volatile std::sig_atomic_t g_running = 1;

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  if (!CreateWindow())
    return EXIT_FAILURE;
  if (!CreateBuffers())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "xdg-ssd: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "xdg-ssd: registry creation failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry&, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using namespace wayland::client;
    using namespace xdg_shell::client;
    using namespace xdg_decoration_unstable_v1::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == zxdg_decoration_manager_v1_traits::interface_name) {
      decoration_mgr_name_ = name;
      decoration_mgr_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "xdg-ssd: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_ || !shm_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "xdg-ssd: required globals not found\n");
    return false;
  }

  // The decoration manager is optional: many compositors (Weston in
  // particular) draw server-side decorations without ever advertising
  // zxdg_decoration_manager_v1, so its absence is reported, not fatal.
  if (!decoration_mgr_name_) {
    std::fprintf(stderr,
                 "xdg-ssd: zxdg_decoration_manager_v1 not advertised — the "
                 "compositor may still draw its own decorations (Weston "
                 "does).\n");
  }
  return true;
}

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace xdg_decoration_unstable_v1::client;

  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "xdg-ssd: wl_compositor bind failed\n");
    return false;
  }

  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "xdg-ssd: wl_shm bind failed\n");
    return false;
  }

  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "xdg-ssd: xdg_wm_base bind failed\n");
    return false;
  }

  // zxdg_decoration_manager_v1 — optional (see ScanGlobals).
  if (decoration_mgr_name_) {
    if (wl_proxy* raw = registry_.Bind<zxdg_decoration_manager_v1_traits>(
            decoration_mgr_name_,
            std::min(decoration_mgr_ver_,
                     zxdg_decoration_manager_v1_traits::version))) {
      decoration_mgr_.Attach(raw);
    } else {
      std::fprintf(stderr, "xdg-ssd: zxdg_decoration_manager_v1 bind failed\n");
      return false;
    }
  }

  // wl_seat — optional, only used for the ESC-to-quit convenience.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "xdg-ssd: wl_seat bind failed\n");
    return false;
  }

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "xdg-ssd: timed out waiting for formats\n");
    return false;
  }

  constexpr uint32_t kXrgb8888 = 1u;
  if (!(shm_.Get()->formats & (1u << kXrgb8888))) {
    std::fprintf(stderr, "xdg-ssd: WL_SHM_FORMAT_XRGB8888 not supported\n");
    return false;
  }
  return true;
}

bool App::CreateWindow() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace xdg_decoration_unstable_v1::client;

  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "xdg-ssd: wl_compositor.create_surface failed\n");
    return false;
  }

  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "xdg-ssd: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "xdg-ssd: xdg_surface.get_toplevel failed\n");
    return false;
  }
  auto* toplevel = xdg_toplevel_.Get();
  toplevel->app_ = this;
  toplevel->SetTitle(title_);
  toplevel->SetAppId("org.wayland-cxx.xdg-ssd");

  // Create the toplevel decoration object and request server-side mode — only
  // when the compositor offers the manager. Otherwise rely on whatever frame
  // the compositor draws on its own (Weston always does).
  if (!decoration_mgr_.IsNull()) {
    if (wl_proxy* raw = wl::construct<
            zxdg_toplevel_decoration_v1_traits,
            zxdg_decoration_manager_v1_traits::Op::GetToplevelDecoration>(
            *decoration_mgr_.Get(), xdg_toplevel_.Get()->GetProxy())) {
      if (!wl::SetupHandler(decoration_, raw)) {
        std::fprintf(stderr, "xdg-ssd: decoration handler setup failed\n");
        return false;
      }
      decoration_.Get()->app_ = this;
      decoration_.Get()->SetMode(
          static_cast<uint32_t>(ZxdgToplevelDecorationV1Mode::ServerSide));
    } else {
      std::fprintf(stderr, "xdg-ssd: get_toplevel_decoration failed\n");
      return false;
    }
  }

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "xdg-ssd: timed out waiting for configure\n");
    return false;
  }
  return true;
}

bool App::CreateBuffers() {
  return pool_.Create(width_, height_, shm_.Get()->GetProxy());
}

// ── Callbacks ─────────────────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(int32_t width, int32_t height) {
  // The compositor reports the content size (decorations are outside it).
  if (width > 0 && height > 0) {
    const int w = std::clamp(width, 1, kMaxDim);
    const int h = std::clamp(height, 1, kMaxDim);
    if (w != width_ || h != height_) {
      width_ = w;
      height_ = h;
      geometry_dirty_ = true;
    }
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnDecorationConfigure(uint32_t mode) {
  using xdg_decoration_unstable_v1::client::ZxdgToplevelDecorationV1Mode;
  const bool server_side =
      (mode == static_cast<uint32_t>(ZxdgToplevelDecorationV1Mode::ServerSide));
  std::fprintf(stderr, "xdg-ssd: compositor chose %s decorations%s\n",
               server_side ? "server-side" : "client-side",
               server_side ? "" : " (request declined)");
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.key == KEY_ESC && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

void App::OnFrameDone(uint32_t stamp_ms) noexcept {
  wl_proxy* const spent = frame_cb_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent)
      wl_proxy_destroy(spent);
  }};

  RequestFrameCallback();
  CommitFrame(stamp_ms);
}

// ── Frame commit ──────────────────────────────────────────────────────────

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_cb_.Get()->app_ = this;
    frame_cb_.Get()->_SetProxy(raw);
  }
}

void App::CommitFrame(uint32_t time_ms) noexcept {
  if (pool_.width != width_ || pool_.height != height_) {
    if (!pool_.Recreate(width_, height_, shm_.Get()->GetProxy())) {
      std::fprintf(stderr,
                   "xdg-ssd: buffer reallocation failed — skipping frame\n");
      return;
    }
    geometry_dirty_ = true;
  }

  const int idx = pool_.NextFree();
  if (idx < 0) {
    std::fprintf(stderr, "xdg-ssd: all buffers busy — skipping frame\n");
    return;
  }

  auto* pixels = static_cast<uint32_t*>(pool_.PixelData(idx));
  const std::size_t npixels =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  paint_content({pixels, npixels}, width_, height_, time_ms);

  // The whole surface is content; the compositor draws decorations outside it.
  // Geometry only changes on resize, so send it once rather than every frame.
  if (geometry_dirty_) {
    xdg_surface_.Get()->SetWindowGeometry(0, 0, width_, height_);
    geometry_dirty_ = false;
  }

  surface_.Get()->Attach(
      pool_.bufs.at(static_cast<std::size_t>(idx)).Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, width_, height_);
  surface_.Get()->Commit();
  pool_.bufs.at(static_cast<std::size_t>(idx)).Get()->busy = true;
}

// ── Teardown + main loop ───────────────────────────────────────────────────

App::~App() {
  seat_.Release();
  // Destroy the decoration before the toplevel (protocol requirement).
  decoration_.Reset();
}

bool App::MainLoop() {
  std::fprintf(stderr,
               "xdg-ssd: %dx%d, %s (press ESC or close the window to quit)\n",
               width_, height_,
               decoration_mgr_.IsNull()
                   ? "no decoration manager — relying on compositor frame"
                   : "requested server-side decorations");

  RequestFrameCallback();
  CommitFrame(0);

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; }, "xdg-ssd",
      [this] { return seat_.GetRepeatFd(); },
      [this] { seat_.DispatchRepeat(); });

  std::fprintf(stderr, "xdg-ssd exiting\n");
  return ok;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

static void signal_handler(int /*sig*/) noexcept {
  g_running = 0;
}

static void print_usage(const char* prog) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "  -w WIDTH   Window width (default: 600)\n"
               "  -h HEIGHT  Window height (default: 400)\n"
               "  -t TITLE   Window title (default: xdg-ssd demo)\n",
               prog);
}

int main(const int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);

  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);

  const std::vector<std::string_view> args(argv, std::next(argv, argc));

  int width = 600;
  int height = 400;
  const char* title = "xdg-ssd demo";

  const auto parse_int_arg = [&](const char* flag, std::string_view val_str,
                                 int& out) -> bool {
    char* end = nullptr;
    errno = 0;
    const long val = std::strtol(val_str.data(), &end, 10);
    if (errno == ERANGE || end == val_str.data() || *end != '\0' || val <= 0 ||
        val > kMaxDim) {
      std::fprintf(stderr, "xdg-ssd: invalid %s value '%.*s' (must be 1..%d)\n",
                   flag, static_cast<int>(val_str.size()), val_str.data(),
                   kMaxDim);
      print_usage(args.at(0).data());
      return false;
    }
    out = static_cast<int>(val);
    return true;
  };

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (const auto& arg = args.at(i); arg == "-w" && i + 1 < args.size()) {
      if (!parse_int_arg("-w", args.at(++i), width))
        return EXIT_FAILURE;
    } else if (arg == "-h" && i + 1 < args.size()) {
      if (!parse_int_arg("-h", args.at(++i), height))
        return EXIT_FAILURE;
    } else if (arg == "-t" && i + 1 < args.size()) {
      title = args.at(++i).data();
    } else {
      print_usage(args.at(0).data());
      return EXIT_FAILURE;
    }
  }

  App app{width, height, title};
  return app.Run();
}

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-pro-type-reinterpret-cast)
