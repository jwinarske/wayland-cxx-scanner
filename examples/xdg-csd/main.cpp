// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// xdg-csd — Client-Side Decoration example with plugin architecture
//
// Demonstrates the zxdg_decoration_manager_v1 protocol for negotiating
// CSD vs SSD with the compositor, and renders client-side decorations
// (title bar with close/maximize/minimize buttons, resize borders)
// using a pluggable CSD rendering backend.
//
// Following the plugin pattern from libdecor
// (https://gitlab.freedesktop.org/libdecor/libdecor/-/tree/master/src/plugins/gtk):
//   • GtkCsdPlugin      — GTK-themed decorations via Cairo/Pango (optional)
//   • FallbackCsdPlugin  — flat-color SHM decorations (always available)
//
// The build system selects the GTK plugin when gtk+-3.0 is available,
// otherwise falls back to the regular plugin.
//
// This example provides equivalent functionality to libdecor's core
// decoration features:
//   • Decoration mode negotiation via xdg-decoration-unstable-v1
//   • Title bar rendering with window control buttons
//   • Resize borders around the window
//   • Interactive move (click title bar), resize (click border),
//     and close (click close button) via pointer events
//   • Proper xdg_surface.set_window_geometry to exclude decorations
//
// Usage:
//   xdg_csd [-w WIDTH] [-h HEIGHT] [-t TITLE]

// clang-tidy: suppress diagnostics common to Wayland C-API boundary code.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-pro-bounds-pointer-arithmetic,
//             cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//             cppcoreguidelines-pro-bounds-constant-array-index,
//             cppcoreguidelines-pro-type-reinterpret-cast)

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "wayland_client.hpp"                     // namespace wayland::client
#include "xdg_decoration_unstable_v1_client.hpp"  // namespace xdg_decoration_unstable_v1::client
#include "xdg_shell_client.hpp"                   // namespace xdg_shell::client

// ── Framework headers ────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/csd_plugin.hpp>
#include <wl/cursor.hpp>
// The fallback is always available and always compiled in: it is both the
// build-time choice when nothing richer is present, and the run-time landing
// spot when a themed plugin declines to start.
#include <wl/csd_fallback.hpp>
#ifdef USE_GTK_CSD
#include <wl/csd_gtk.hpp>
#elif defined(USE_CAIRO_CSD)
#include <wl/csd_cairo.hpp>
#endif
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>
#include <vector>
#include <wl/span.hpp>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() — core Wayland interfaces
//
// The wl_iface() definitions for every interface SeatManager binds (wl_seat,
// wl_keyboard, wl_pointer, wl_touch) are provided inline by <wl/seat.hpp>.
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
const wl_interface& wl_region_traits::wl_iface() noexcept {
  return wl_region_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// CSD types — provided by the plugin interface in <wl/csd_plugin.hpp>
// ══════════════════════════════════════════════════════════════════════════════

using wl::csd::CsdPlugin;
using wl::csd::HitZone;

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
    fd = memfd_create("xdg-csd", 0);
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

// ── WlCompositorHandler ─────────────────────────────────────────────────────

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
};

// ── WlShmPoolHandler ────────────────────────────────────────────────────────

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
 public:
};

// ── WlShmHandler ────────────────────────────────────────────────────────────

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  uint32_t formats = 0;
  void OnFormat(uint32_t fmt) override {
    if (fmt < 32u)
      formats |= (1u << fmt);
  }
};

// ── WlBufferHandler ─────────────────────────────────────────────────────────

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool busy = false;
  void OnRelease() override { busy = false; }
};

// ── WlSurfaceHandler ────────────────────────────────────────────────────────

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// ── WlRegionHandler ─────────────────────────────────────────────────────────

class WlRegionHandler : public wayland::client::CWlRegion<WlRegionHandler> {};

// ── WlCallbackHandler ───────────────────────────────────────────────────────

class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// ── XDG shell handlers provided by <wl/xdg_shell.hpp> ──────────────────────
//   wl::XdgWmBaseHandler        — responds to ping automatically
//   wl::XdgSurfaceHandler<App>  — acks configure, calls OnXdgSurfaceConfigure
//   wl::XdgToplevelHandler<App> — delegates configure/close to App

// ── XDG decoration handlers provided by <wl/xdg_decoration.hpp> ─────────────
//   wl::XdgDecorationHandler<App>   — delegates configure to App

// ── Seat/keyboard/pointer handled by wl::SeatManager<App> (<wl/seat.hpp>) ────
//   Binds the keyboard and — because App defines OnPointer* hooks — the
//   pointer, dispatching typed events to the App's hooks below.

// ══════════════════════════════════════════════════════════════════════════════
// Buffer pool — pre-allocates 2 double-buffered wl_shm buffers
// ══════════════════════════════════════════════════════════════════════════════

static constexpr int kNumBuffers = 2;

struct BufferPool {
  ShmMapping mem;
  std::array<wl::WlPtr<WlBufferHandler>, static_cast<std::size_t>(kNumBuffers)>
      bufs;
  int next = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] bool Create(int w, int h, wl_proxy* shm_raw) noexcept;

  void Recreate(int w, int h, wl_proxy* shm_raw) noexcept {
    for (auto& b : bufs)
      b.Reset();
    mem.Reset();
    next = 0;
    static_cast<void>(Create(w, h, shm_raw));
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

  const std::size_t stride = static_cast<std::size_t>(w) * 4u;
  const std::size_t per_buf = stride * static_cast<std::size_t>(h);
  const std::size_t total = per_buf * static_cast<std::size_t>(kNumBuffers);

  if (!mem.Create(total)) {
    std::fprintf(stderr, "xdg-csd: SHM allocation failed\n");
    return false;
  }

  wl::WlPtr<WlShmPoolHandler> pool;
  {
    wl_shm_pool* raw_pool = wl_shm_create_pool(
        reinterpret_cast<wl_shm*>(shm_raw), mem.fd, static_cast<int>(total));
    if (!raw_pool) {
      std::fprintf(stderr, "xdg-csd: wl_shm_create_pool failed\n");
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
            WL_SHM_FORMAT_ARGB8888)) {
      bufs.at(static_cast<std::size_t>(i)).Get()->_SetProxy(raw);
    } else {
      std::fprintf(stderr, "xdg-csd: wl_shm_pool.create_buffer [%d] failed\n",
                   i);
      return false;
    }
  }

  pool.Reset();
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Pixel painting
// ══════════════════════════════════════════════════════════════════════════════

/// Paint the application's content — an animated ring pattern.
///
/// This is the app's own drawing, deliberately kept out of the CSD plugins:
/// a decoration plugin owns the chrome and nothing else.  The content is
/// written at (@p dst_x, @p dst_y) into a buffer of @p stride pixels per row,
/// so the same painter serves both the CSD case (content inset by the
/// decoration margins) and the SSD case (content fills the surface).
///
/// Pixels are opaque ARGB8888: the alpha channel exists for the decoration's
/// benefit, not the content's.
static void paint_content(wl::span<uint32_t> buf,
                          int dst_x,
                          int dst_y,
                          int width,
                          int height,
                          int stride,
                          uint32_t time) noexcept {
  const int halfh = height / 2;
  const int halfw = width / 2;
  int64_t outer_r = (halfw < halfh ? halfw : halfh) - 8;
  const int64_t inner_r = outer_r - 32;
  outer_r *= outer_r;
  const int64_t inner_r2 = inner_r * inner_r;

  for (int y = 0; y < height; ++y) {
    const int64_t oy = y - halfh;
    const int64_t y2 = oy * oy;
    for (int x = 0; x < width; ++x) {
      uint32_t v;
      const int64_t ox = x - halfw;
      const int64_t r2 = ox * ox + y2;
      if (r2 < inner_r2)
        v = (static_cast<uint32_t>(r2 / 32) + time / 64) * 0x0080401u;
      else if (r2 < outer_r)
        v = (static_cast<uint32_t>(y) + time / 32) * 0x0080401u;
      else
        v = (static_cast<uint32_t>(x) + time / 16) * 0x0080401u;
      // Opaque: the diagonal cross that the classic demo leaves transparent
      // would need premultiplied RGB ≤ alpha to be valid, and the content has
      // no reason to be see-through.
      v = (v & 0x00FFFFFFu) | 0xFF000000u;
      const std::size_t idx = static_cast<std::size_t>(dst_y + y) *
                                  static_cast<std::size_t>(stride) +
                              static_cast<std::size_t>(dst_x + x);
      buf[idx] = v;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  App(int content_w,
      int content_h,
      const char* title,
      std::unique_ptr<CsdPlugin> plugin)
      : content_w_(content_w),
        content_h_(content_h),
        title_(title),
        csd_plugin_(std::move(plugin)) {
    if (csd_plugin_)
      csd_plugin_->SetTitle(title);
  }
  ~App();

  int Run();

  // ── Callbacks from CRTP handlers ──────────────────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t width, int32_t height);
  void OnToplevelStates(const wl::ToplevelStates& states);
  void OnToplevelClose();
  void OnDecorationConfigure(uint32_t mode);
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameDone(uint32_t stamp_ms) noexcept;

  // ── Pointer callbacks (dispatched by wl::SeatManager) ─────────────────────
  void OnPointerEnter(const wl::PointerEvent& ev) noexcept;
  void OnPointerLeave() noexcept;
  void OnPointerMotion(const wl::PointerEvent& ev) noexcept;
  void OnPointerButton(const wl::PointerButtonEvent& ev) noexcept;

 private:
  // ── Configuration ─────────────────────────────────────────────────────────
  int content_w_;
  int content_h_;
  const char* title_;

  // ── Decoration state ──────────────────────────────────────────────────────
  bool use_csd_ = true;  // default to CSD if no decoration manager
  bool maximized_ = false;

  // ── CSD plugin (fallback or GTK-themed) ───────────────────────────────
  std::unique_ptr<CsdPlugin> csd_plugin_;

  // ── Computed surface dimensions ───────────────────────────────────────────
  [[nodiscard]] int SurfaceWidth() const noexcept {
    return use_csd_ && csd_plugin_ ? csd_plugin_->SurfaceWidth(content_w_)
                                   : content_w_;
  }
  [[nodiscard]] int SurfaceHeight() const noexcept {
    return use_csd_ && csd_plugin_ ? csd_plugin_->SurfaceHeight(content_h_)
                                   : content_h_;
  }

  // ── Hit testing ───────────────────────────────────────────────────────────
  [[nodiscard]] HitZone HitTest(int x, int y) const noexcept;

  // Point the cursor at the shape for the zone currently under the pointer.
  void UpdateCursor() noexcept {
    cursor_.Set(seat_.Pointer(), enter_serial_,
                wl::csd::HitZoneToCursorName(HitTest(pointer_x_, pointer_y_)));
  }

  // ── Wayland objects ───────────────────────────────────────────────────────
  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;

  // Optional: xdg-decoration.
  wl::WlPtr<wl::XdgDecorationManagerHandler> decoration_mgr_;
  wl::WlPtr<wl::XdgDecorationHandler<App>> decoration_;

  // Input: seat + keyboard + pointer, all owned by SeatManager.
  wl::SeatManager<App> seat_;
  // Cursor for the seat's pointer (SeatManager owns the pointer but not the
  // cursor); the shape follows the hit-tested decoration zone.
  wl::CursorManager cursor_;

  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  wl::WlPtr<WlCallbackHandler> frame_cb_;
  BufferPool pool_;

  // ── Application state ─────────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  bool need_redraw_ = true;
  uint32_t last_time_ = 0;

  // ── Pointer state ─────────────────────────────────────────────────────────
  int pointer_x_ =
      -1;  // -1 ⇒ pointer not over the surface (wl::csd::InputState)
  int pointer_y_ = -1;
  bool pointer_pressed_ = false;
  uint32_t enter_serial_ = 0;  // last wl_pointer.enter serial, for set_cursor

  // Driven by the xdg_toplevel configure `states` array, not by keyboard focus:
  // ACTIVATED is what the compositor means by "this window is the active one",
  // and it is what the decoration's active/backdrop styling must follow.
  bool focused_ = true;

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
  void UpdateOpaqueRegion(int x, int y, int w, int h) noexcept;

  // Last opaque region submitted, to avoid re-sending it every frame.
  int opaque_x_ = -1, opaque_y_ = -1, opaque_w_ = -1, opaque_h_ = -1;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler implementations (need full App definition)
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

// ── ConnectDisplay ──────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "xdg-csd: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals ─────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "xdg-csd: registry creation failed\n");
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
    std::fprintf(stderr, "xdg-csd: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_ || !shm_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "xdg-csd: required globals not found\n");
    return false;
  }
  return true;
}

// ── BindGlobals ─────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace xdg_decoration_unstable_v1::client;

  // wl_compositor — no events.
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "xdg-csd: wl_compositor bind failed\n");
    return false;
  }

  // wl_shm.
  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "xdg-csd: wl_shm bind failed\n");
    return false;
  }

  // xdg_wm_base.
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "xdg-csd: xdg_wm_base bind failed\n");
    return false;
  }

  // zxdg_decoration_manager_v1 — optional.
  if (decoration_mgr_name_) {
    if (wl_proxy* raw = registry_.Bind<zxdg_decoration_manager_v1_traits>(
            decoration_mgr_name_,
            std::min(decoration_mgr_ver_,
                     zxdg_decoration_manager_v1_traits::version))) {
      decoration_mgr_.Attach(raw);
    }
  }
  if (decoration_mgr_.IsNull()) {
    std::fprintf(stderr,
                 "xdg-csd: zxdg_decoration_manager_v1 not available — "
                 "falling back to client-side decorations\n");
  }

  // wl_seat — SeatManager binds the keyboard and, since App defines OnPointer*
  // hooks, the pointer too, on the seat capabilities event.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "xdg-csd: wl_seat bind failed\n");
    return false;
  }

  // Roundtrip to receive formats and capabilities.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "xdg-csd: timed out waiting for formats\n");
    return false;
  }

  // ARGB8888 — decorations need an alpha channel.  wl_shm guarantees this
  // format, but check rather than assume.
  constexpr uint32_t kArgb8888 = 0u;
  if (!(shm_.Get()->formats & (1u << kArgb8888))) {
    std::fprintf(stderr, "xdg-csd: WL_SHM_FORMAT_ARGB8888 not supported\n");
    return false;
  }

  // Load the cursor theme; optional — decorations still work without a cursor.
  if (!cursor_.Init(shm_.Get()->GetProxy(), compositor_.Get()->GetProxy())) {
    std::fprintf(stderr, "xdg-csd: cursor theme unavailable (no set_cursor)\n");
  }
  return true;
}

// ── CreateWindow ────────────────────────────────────────────────────────────

bool App::CreateWindow() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace xdg_decoration_unstable_v1::client;

  // wl_surface.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "xdg-csd: wl_compositor.create_surface failed\n");
    return false;
  }

  // xdg_surface.
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "xdg-csd: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // xdg_toplevel.
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "xdg-csd: xdg_surface.get_toplevel failed\n");
    return false;
  }
  auto* toplevel = xdg_toplevel_.Get();
  toplevel->app_ = this;
  toplevel->SetTitle(title_);
  toplevel->SetAppId("org.wayland-cxx.xdg-csd");

  // Negotiate decoration mode via zxdg_decoration_manager_v1.
  if (!decoration_mgr_.IsNull()) {
    if (wl_proxy* raw = wl::construct<
            zxdg_toplevel_decoration_v1_traits,
            zxdg_decoration_manager_v1_traits::Op::GetToplevelDecoration>(
            *decoration_mgr_.Get(), xdg_toplevel_.Get()->GetProxy())) {
      if (wl::SetupHandler(decoration_, raw)) {
        decoration_.Get()->app_ = this;
        // Request client-side decorations.
        decoration_.Get()->SetMode(
            static_cast<uint32_t>(ZxdgToplevelDecorationV1Mode::ClientSide));
      }
    }
  }

  // Commit to trigger the configure sequence.
  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "xdg-csd: timed out waiting for configure\n");
    return false;
  }

  return true;
}

// ── CreateBuffers ───────────────────────────────────────────────────────────

bool App::CreateBuffers() {
  return pool_.Create(SurfaceWidth(), SurfaceHeight(), shm_.Get()->GetProxy());
}

// ── Hit testing ─────────────────────────────────────────────────────────────

HitZone App::HitTest(int x, int y) const noexcept {
  if (!use_csd_ || !csd_plugin_)
    return HitZone::Content;

  return csd_plugin_->HitTest(x, y, SurfaceWidth(), SurfaceHeight(), content_w_,
                              content_h_);
}

// ── Callback implementations ────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
  need_redraw_ = true;
}

void App::OnToplevelConfigure(int32_t width, int32_t height) {
  if (width > 0 && height > 0) {
    // The compositor provides the total window size.
    // In CSD mode, subtract decoration space to get the content area.
    if (use_csd_ && csd_plugin_) {
      const wl::csd::Margins m = csd_plugin_->DecorationMargins();
      content_w_ = width - m.left - m.right;
      content_h_ = height - m.top - m.bottom;
    } else {
      content_w_ = width;
      content_h_ = height;
    }
    static constexpr int kMaxDim = 16384;
    content_w_ = std::clamp(content_w_, 1, kMaxDim);
    content_h_ = std::clamp(content_h_, 1, kMaxDim);
    need_redraw_ = true;
  }
}

// The compositor is the authority on both of these. Tracking them from our own
// button clicks instead would be optimistic: a maximize can be refused, and can
// equally arrive from a keybinding or a double-click we never saw.
void App::OnToplevelStates(const wl::ToplevelStates& states) {
  focused_ = states.activated;
  maximized_ = states.maximized;
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnDecorationConfigure(uint32_t mode) {
  const bool was_csd = use_csd_;
  use_csd_ = (mode == static_cast<uint32_t>(
                          xdg_decoration_unstable_v1::client::
                              ZxdgToplevelDecorationV1Mode::ClientSide));
  if (was_csd != use_csd_) {
    need_redraw_ = true;
    std::fprintf(stderr, "xdg-csd: decoration mode → %s\n",
                 use_csd_ ? "client-side" : "server-side");
  }
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.key == KEY_ESC && ev.state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

void App::OnFrameDone(const uint32_t stamp_ms) noexcept {
  wl_proxy* const spent = frame_cb_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent)
      wl_proxy_destroy(spent);
  }};

  last_time_ = stamp_ms;
  RequestFrameCallback();
  CommitFrame(stamp_ms);
}

// ── Pointer event implementations ───────────────────────────────────────────

void App::OnPointerEnter(const wl::PointerEvent& ev) noexcept {
  enter_serial_ = ev.serial;  // set_cursor must carry an enter serial
  pointer_x_ = static_cast<int>(ev.x);
  pointer_y_ = static_cast<int>(ev.y);
  cursor_.Reset();  // the compositor resets the cursor on enter; re-apply
  UpdateCursor();
}

void App::OnPointerLeave() noexcept {
  pointer_x_ = -1;
  pointer_y_ = -1;
  pointer_pressed_ = false;
}

void App::OnPointerMotion(const wl::PointerEvent& ev) noexcept {
  pointer_x_ = static_cast<int>(ev.x);
  pointer_y_ = static_cast<int>(ev.y);
  UpdateCursor();
}

void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  if (ev.button != BTN_LEFT)
    return;

  // Track the held state so the plugin can render a pressed button.  No
  // redraw is requested: the frame callback already redraws unconditionally.
  pointer_pressed_ = (ev.state == WL_POINTER_BUTTON_STATE_PRESSED);

  if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED)
    return;

  // The seat proxy backs interactive move/resize; the button serial authorizes
  // the grab (see wl::SeatManager::Seat()).
  wl_proxy* const seat = seat_.Seat();
  const HitZone zone = HitTest(pointer_x_, pointer_y_);

  switch (zone) {
    case HitZone::TitleBar:
      // Interactive move.
      if (seat != nullptr)
        xdg_toplevel_.Get()->Move(seat, ev.serial);
      break;

    case HitZone::CloseButton:
      running_ = false;
      break;

    case HitZone::MaximizeButton:
      // Request only. maximized_ follows the configure the compositor sends
      // back, so a refused request does not leave us drawing the wrong icon.
      if (maximized_)
        xdg_toplevel_.Get()->UnsetMaximized();
      else
        xdg_toplevel_.Get()->SetMaximized();
      break;

    case HitZone::MinimizeButton:
      xdg_toplevel_.Get()->SetMinimized();
      break;

    default: {
      // Resize zones.
      const uint32_t edge = wl::csd::HitZoneToResizeEdge(zone);
      if (edge != 0 && seat != nullptr)
        xdg_toplevel_.Get()->Resize(seat, ev.serial, edge);
    } break;
  }
}

// ── Frame commit ────────────────────────────────────────────────────────────

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_cb_.Get()->app_ = this;
    frame_cb_.Get()->_SetProxy(raw);
  }
}

// Tell the compositor which part of the surface is fully opaque, so it can
// skip blending there.  The buffer is ARGB8888 — without this the compositor
// must assume every pixel may be translucent and blend the whole surface.
//
// Re-sent only when the rectangle changes; a wl_region is a throwaway object,
// so it is created, populated, installed, and destroyed in one go.
void App::UpdateOpaqueRegion(int x, int y, int w, int h) noexcept {
  if (x == opaque_x_ && y == opaque_y_ && w == opaque_w_ && h == opaque_h_)
    return;

  using wayland::client::wl_compositor_traits;
  using wayland::client::wl_region_traits;

  // wl_region has no events, so it takes a bare Attach() rather than
  // _SetProxy() (which would install a dispatcher the interface never
  // generates).  WlPtr is RAII: leaving scope sends destroy for us.
  wl::WlPtr<WlRegionHandler> region;
  wl_proxy* const raw =
      wl::construct<wl_region_traits, wl_compositor_traits::Op::CreateRegion>(
          *compositor_.Get());
  if (!raw)
    return;
  region.Attach(raw);

  region.Get()->Add(x, y, w, h);
  surface_.Get()->SetOpaqueRegion(region.Get()->GetProxy());

  opaque_x_ = x;
  opaque_y_ = y;
  opaque_w_ = w;
  opaque_h_ = h;
}

void App::CommitFrame(uint32_t time_ms) noexcept {
  const int sw = SurfaceWidth();
  const int sh = SurfaceHeight();

  // Recreate buffers if size changed.
  if (pool_.width != sw || pool_.height != sh) {
    pool_.Recreate(sw, sh, shm_.Get()->GetProxy());
  }

  const int idx = pool_.NextFree();
  if (idx < 0) {
    std::fprintf(stderr, "xdg-csd: all buffers busy — skipping frame\n");
    return;
  }

  auto* pixels = static_cast<uint32_t*>(pool_.PixelData(idx));
  const std::size_t npixels =
      static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh);

  if (use_csd_ && csd_plugin_) {
    // Let the plugin pump its own event source (theme changes, etc.) before it
    // is asked for geometry or pixels.  This is the app's only chance: the
    // event loop blocks in poll() on the Wayland fd, so the plugin is drained
    // once per frame — which is enough precisely because the demo animates and
    // therefore always has a next frame.
    csd_plugin_->Dispatch();

    csd_plugin_->SetInputState(
        {pointer_x_, pointer_y_, pointer_pressed_, focused_, maximized_});

    const wl::csd::Margins m = csd_plugin_->DecorationMargins();

    // The plugin paints the chrome; the app paints its own content into the
    // content rect the chrome leaves untouched.
    csd_plugin_->RenderDecoration(pixels, sw, sh, content_w_, content_h_);
    paint_content({pixels, npixels}, m.left, m.top, content_w_, content_h_, sw,
                  time_ms);

    // Window geometry excludes the decoration area.
    xdg_surface_.Get()->SetWindowGeometry(m.left, m.top, content_w_,
                                          content_h_);

    // Everything below the title bar is opaque, but the title bar itself is
    // not: a themed decoration rounds its top corners, leaving those pixels
    // transparent.  Claiming they are opaque would stop the compositor
    // blending them and the rounding would come out as black corners, so the
    // region stops below the title bar and that band is left to blend.
    UpdateOpaqueRegion(0, m.top, sw, sh - m.top);
  } else {
    paint_content({pixels, npixels}, 0, 0, sw, sh, sw, time_ms);
    UpdateOpaqueRegion(0, 0, sw, sh);
  }

  surface_.Get()->Attach(
      pool_.bufs.at(static_cast<std::size_t>(idx)).Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, sw, sh);
  surface_.Get()->Commit();
  pool_.bufs.at(static_cast<std::size_t>(idx)).Get()->busy = true;
}

// ── MainLoop ────────────────────────────────────────────────────────────────

App::~App() {
  // Release keyboard + pointer (SeatManager sends the versioned releases)
  // before the WlPtr members are destroyed.
  seat_.Release();
  // Destroy decoration before toplevel (protocol requirement).
  decoration_.Reset();
}

bool App::MainLoop() {
  std::fprintf(stderr,
               "xdg-csd: %dx%d content, decorations=%s "
               "(press ESC or click ✕ to quit)\n",
               content_w_, content_h_, use_csd_ ? "CSD" : "SSD");

  // Kickstart: request the first frame callback, then commit.
  RequestFrameCallback();
  CommitFrame(0);

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; }, "xdg-csd",
      [this] { return seat_.GetRepeatFd(); },
      [this] { seat_.DispatchRepeat(); });

  std::fprintf(stderr, "xdg-csd exiting\n");
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
               "  -w WIDTH   Content width (default: 400)\n"
               "  -h HEIGHT  Content height (default: 300)\n"
               "  -t TITLE   Window title (default: xdg-csd demo)\n",
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

  int content_w = 400;
  int content_h = 300;
  const char* title = "xdg-csd demo";

  // Helper: parse a positive integer argument for the given option flag.
  const auto parse_int_arg = [&](const char* flag, std::string_view val_str,
                                 int& out) -> bool {
    char* end = nullptr;
    errno = 0;
    const long val = std::strtol(val_str.data(), &end, 10);
    if (errno == ERANGE || end == val_str.data() || *end != '\0' || val <= 0 ||
        val > INT_MAX) {
      std::fprintf(stderr, "xdg-csd: invalid %s value '%.*s'\n", flag,
                   static_cast<int>(val_str.size()), val_str.data());
      print_usage(args.at(0).data());
      return false;
    }
    out = static_cast<int>(val);
    return true;
  };

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (const auto& arg = args.at(i); arg == "-w" && i + 1 < args.size()) {
      if (!parse_int_arg("-w", args.at(++i), content_w))
        return EXIT_FAILURE;
    } else if (arg == "-h" && i + 1 < args.size()) {
      if (!parse_int_arg("-h", args.at(++i), content_h))
        return EXIT_FAILURE;
    } else if (arg == "-t" && i + 1 < args.size()) {
      title = args.at(++i).data();
    } else {
      print_usage(args.at(0).data());
      return EXIT_FAILURE;
    }
  }

  // Which plugin is compiled in is a build-time choice (the csd / csd_gtk
  // options). The themed plugin can still fail at run time — GTK may be linked
  // but have no usable display or theme — so it is asked rather than assumed,
  // and the dependency-free fallback covers the refusal.
  std::unique_ptr<CsdPlugin> plugin;
#ifdef USE_GTK_CSD
  plugin = wl::csd::GtkCsdPlugin::TryCreate();
  if (plugin) {
    std::fprintf(stderr, "xdg-csd: using GTK CSD plugin\n");
  } else {
    std::fprintf(stderr,
                 "xdg-csd: GTK unavailable at run time — using fallback CSD "
                 "plugin\n");
  }
#elif defined(USE_CAIRO_CSD)
  plugin = std::make_unique<wl::csd::CairoCsdPlugin>();
  std::fprintf(stderr, "xdg-csd: using Cairo CSD plugin\n");
#endif

  if (!plugin) {
    plugin = std::make_unique<wl::csd::FallbackCsdPlugin>();
#if !defined(USE_GTK_CSD) && !defined(USE_CAIRO_CSD)
    std::fprintf(stderr, "xdg-csd: using fallback CSD plugin\n");
#endif
  }

  App app{content_w, content_h, title, std::move(plugin)};
  return app.Run();
}

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//           cppcoreguidelines-pro-bounds-constant-array-index,
//           cppcoreguidelines-pro-type-reinterpret-cast)
