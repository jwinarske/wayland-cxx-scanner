// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// xdg-csd — a decorated window, drawn with wl_shm
//
// A window with a frame around it, where the frame is wl::csd::DecoratedWindow
// and the content is this example's own animated ring, painted into a wl_shm
// buffer.  What this shows is how little a decorated client has to do: hand the
// frame the window, forward the pointer, and commit the frame before the
// content.  Everything a decoration involves — which plugin draws it, whether
// the compositor would rather draw it instead (xdg-decoration-unstable-v1), the
// title bar, the buttons, the resize edges, the drag and double-click gestures,
// and the window geometry the compositor sizes against — is the frame's, behind
// that interface.  The csd option decides which plugin exists, and csd=ssd
// leaves none: the negotiation then asks the compositor to decorate and this
// draws nothing but its content.  See examples/csd-common/decorated_window.hpp.
//
// The surface here is the content and nothing else.  The frame hangs the
// decoration on a subsurface behind it, so no margin arithmetic, no offset
// content, and no geometry to declare appear anywhere below — which is the
// point, and is what an EGL or Vulkan client needs, having no CPU buffer for a
// plugin to paint into at all.  simple-egl is the same frame on that path.
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
#include "fractional_scale_client.hpp"  // namespace fractional_scale_v1::client
#include "viewporter_client.hpp"        // namespace viewporter::client
#include "wayland_client.hpp"           // namespace wayland::client
#include "xdg_shell_client.hpp"         // namespace xdg_shell::client

// ── Framework headers ────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/cursor.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/scale_policy.hpp>  // wl::ScalePolicy — buffer/viewport sizing
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>
// The window frame: it owns the decoration, the decoration negotiation and the
// gestures that drive them, on a subsurface of this example's content surface.
// Not a framework header — it needs a .cpp and a toolkit, which a header-only
// framework cannot carry, so it comes from csd_dep with the plugins.
#include "decorated_window.hpp"

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
// The window frame's, not this example's: it puts the decoration on a
// subsurface. Defined here because the repo defines wl_iface() per consumer, so
// the frame cannot define them without colliding with whatever links it.
const wl_interface& wl_subcompositor_traits::wl_iface() noexcept {
  return wl_subcompositor_interface;
}
const wl_interface& wl_subsurface_traits::wl_iface() noexcept {
  return wl_subsurface_interface;
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

// ── Scale: viewporter + fractional-scale ────────────────────────────────────
// wp_viewporter / wp_viewport and the fractional-scale manager have no events.

class WpViewporterHandler
    : public viewporter::client::CWpViewporter<WpViewporterHandler> {};
class WpViewportHandler
    : public viewporter::client::CWpViewport<WpViewportHandler> {};
class WpFractionalScaleManagerHandler
    : public fractional_scale_v1::client::CWpFractionalScaleManagerV1<
          WpFractionalScaleManagerHandler> {};

// wp_fractional_scale_v1 delivers the compositor's preferred scale in 1/120
// units via preferred_scale.
class WpFractionalScaleHandler
    : public fractional_scale_v1::client::CWpFractionalScaleV1<
          WpFractionalScaleHandler> {
 public:
  App* app_ = nullptr;
  void OnPreferredScale(uint32_t scale_120) override;
};

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

  // True once the compositor has handed every buffer back, so the pool can be
  // torn down without pulling a buffer out from under the surface.
  [[nodiscard]] bool AllReleased() const noexcept {
    for (const auto& b : bufs) {
      if (!b.IsNull() && b.Get()->busy)
        return false;
    }
    return true;
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
  App(int content_w, int content_h, const char* title)
      : content_w_(content_w), content_h_(content_h), title_(title) {}
  ~App();

  int Run();

  // ── Callbacks from CRTP handlers ──────────────────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t width, int32_t height);
  void OnToplevelStates(const wl::ToplevelStates& states);
  void OnToplevelClose();
  void OnPreferredScale(int32_t scale_120) noexcept;
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameDone(uint32_t stamp_ms) noexcept;

  // ── Pointer callbacks (dispatched by wl::SeatManager) ─────────────────────
  void OnPointerEnter(const wl::PointerEvent& ev) noexcept;
  void OnPointerLeave() noexcept;
  void OnPointerMotion(const wl::PointerEvent& ev) noexcept;
  void OnPointerButton(const wl::PointerButtonEvent& ev) noexcept;

 private:
  // ── Configuration ─────────────────────────────────────────────────────────
  // The content area, decoration excluded: the surface is the content now, so
  // this is the surface's size as well. The frame is a subsurface behind it and
  // adds nothing to it.
  int content_w_;
  int content_h_;
  const char* title_;

  // Point the cursor at the shape the frame asks for. Null means the pointer is
  // not over the frame and the application's own cursor applies — which here is
  // the content, so the default arrow.
  void UpdateCursor() noexcept {
    const char* name = frame_.CursorName();
    cursor_.Set(seat_.Pointer(), enter_serial_,
                name != nullptr ? name : "default");
  }

  // ── Wayland objects ───────────────────────────────────────────────────────
  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;

  // Input: seat + keyboard + pointer, all owned by SeatManager.
  wl::SeatManager<App> seat_;
  // Cursor for the seat's pointer (SeatManager owns the pointer but not the
  // cursor); the shape is whichever one the frame asks for.
  wl::CursorManager cursor_;

  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // The window frame, and with it the decoration negotiation: it binds the
  // decoration manager itself, so nothing above is xdg-decoration's.
  //
  // Declared after surface_ and xdg_toplevel_ so it is destroyed before them:
  // it holds a subsurface of surface_ and borrows the toplevel.
  wl::csd::DecoratedWindow frame_;

  wl::WlPtr<WlCallbackHandler> frame_cb_;

  // Optional: viewporter + fractional-scale. Bound as a pair or not at all —
  // a preferred scale with no viewport to present the physical buffer back at
  // the logical size would just make the window the wrong size on screen.
  wl::WlPtr<WpViewporterHandler> viewporter_;
  wl::WlPtr<WpFractionalScaleManagerHandler> fractional_mgr_;
  wl::WlPtr<WpViewportHandler> viewport_;
  wl::WlPtr<WpFractionalScaleHandler> fractional_;

  // The compositor's preferred scale, in 1/120 units (120 = unity). Every
  // other dimension in this file is logical; this is what turns them into the
  // buffer's physical pixels. See <wl/scale_policy.hpp>.
  int32_t scale_120_ = wl::ScalePolicy::kUnityScale120;
  [[nodiscard]] bool CanScale() const noexcept { return !viewport_.IsNull(); }
  // Held by pointer so a resize can hand the old pool aside intact rather than
  // tearing it down underneath the compositor.
  std::unique_ptr<BufferPool> pool_ = std::make_unique<BufferPool>();
  // The pool from before the last resize, kept alive until the compositor has
  // released its buffers.  Destroying a wl_buffer that is still attached
  // leaves the surface contents undefined -- the compositor is entitled to
  // draw whatever it likes, and a compositor that holds buffers across a
  // resize will show exactly that.
  std::unique_ptr<BufferPool> retired_;

  // ── Application state ─────────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  bool need_redraw_ = true;
  uint32_t last_time_ = 0;

  // ── Pointer state ─────────────────────────────────────────────────────────
  // The gestures are the frame's: press-vs-release matching, the drag
  // threshold, the double-click. All that is left here is the serial, which
  // wl_pointer.set_cursor must carry and only the application can answer with.
  uint32_t enter_serial_ = 0;  // last wl_pointer.enter serial, for set_cursor

  // ── Global IDs from registry scan ─────────────────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t viewporter_name_ = 0, viewporter_ver_ = 0;
  uint32_t fractional_mgr_name_ = 0, fractional_mgr_ver_ = 0;

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

  // Last opaque region submitted, to avoid re-sending it every frame: it is
  // double-buffered state that only needs re-declaring when it changes.
  int opaque_x_ = -1, opaque_y_ = -1, opaque_w_ = -1, opaque_h_ = -1;

  // Last viewport destination submitted, same reasoning.
  int viewport_w_ = -1, viewport_h_ = -1;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  app_->OnFrameDone(time_ms);
}

void WpFractionalScaleHandler::OnPreferredScale(uint32_t scale_120) {
  app_->OnPreferredScale(static_cast<int32_t>(scale_120));
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
    using namespace viewporter::client;
    using namespace fractional_scale_v1::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wp_viewporter_traits::interface_name) {
      viewporter_name_ = name;
      viewporter_ver_ = ver;
    } else if (iface == wp_fractional_scale_manager_v1_traits::interface_name) {
      fractional_mgr_name_ = name;
      fractional_mgr_ver_ = ver;
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

  // wp_viewporter + wp_fractional_scale_manager_v1 — optional, and bound as a
  // pair or not at all: a preferred scale is only useful with a viewport to
  // present the physical buffer back at the logical size, and a viewport with
  // no scale to apply has nothing to do.
  if (viewporter_name_ && fractional_mgr_name_) {
    using namespace viewporter::client;
    using namespace fractional_scale_v1::client;
    wl_proxy* vp = registry_.Bind<wp_viewporter_traits>(
        viewporter_name_,
        std::min(viewporter_ver_, wp_viewporter_traits::version));
    wl_proxy* fm = registry_.Bind<wp_fractional_scale_manager_v1_traits>(
        fractional_mgr_name_,
        std::min(fractional_mgr_ver_,
                 wp_fractional_scale_manager_v1_traits::version));
    if (vp != nullptr && fm != nullptr) {
      viewporter_.Attach(vp);
      fractional_mgr_.Attach(fm);
    } else {
      if (vp != nullptr)
        wl_proxy_destroy(vp);
      if (fm != nullptr)
        wl_proxy_destroy(fm);
    }
  }

  // zxdg_decoration_manager_v1 is bound by the frame, from a registry of its
  // own: the negotiation is the frame's, and a manager bound here would be one
  // this example has nothing to do with.

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

  // Per-surface viewport and fractional-scale objects.
  if (!viewporter_.IsNull() && !fractional_mgr_.IsNull()) {
    using namespace viewporter::client;
    using namespace fractional_scale_v1::client;
    if (wl_proxy* raw = wl::construct<wp_viewport_traits,
                                      wp_viewporter_traits::Op::GetViewport>(
            *viewporter_.Get(), surface_.Get()->GetProxy())) {
      viewport_.Attach(raw);
    }
    if (wl_proxy* raw = wl::construct<
            wp_fractional_scale_v1_traits,
            wp_fractional_scale_manager_v1_traits::Op::GetFractionalScale>(
            *fractional_mgr_.Get(), surface_.Get()->GetProxy())) {
      if (wl::SetupHandler(fractional_, raw))
        fractional_.Get()->app_ = this;
    }
  }

  // Hand the frame the connection and the window; it binds wl_subcompositor,
  // its own wl_shm and the decoration manager from a registry of its own, and
  // settles who decorates. Which plugin exists is the csd option's business, so
  // it is asked for rather than named here, and a null plugin is a supported
  // answer rather than an error — that is the csd=ssd case, where the
  // negotiation asks the compositor to decorate and nothing is drawn here.
  //
  // The content size is required: it seeds the size the frame restores to, and
  // a compositor's first configure often has a zero axis meaning "you pick".
  wl::csd::DecoratedWindow::Config cfg;
  cfg.display = display_.Get();
  cfg.content_surface = surface_.Get()->GetProxy();
  cfg.xdg_surface = xdg_surface_.Get()->GetProxy();
  cfg.xdg_toplevel = xdg_toplevel_.Get()->GetProxy();
  cfg.seat = seat_.Seat();
  cfg.content_width = content_w_;
  cfg.content_height = content_h_;
  if (!frame_.Init(cfg, wl::csd::MakeCsdPlugin())) {
    std::fprintf(stderr,
                 "xdg-csd: window frame failed to build; the window will be "
                 "undecorated\n");
  }
  frame_.SetTitle(title_);

  // Commit to trigger the configure sequence.
  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "xdg-csd: timed out waiting for configure\n");
    return false;
  }

  return true;
}

// ── CreateBuffers ───────────────────────────────────────────────────────────

// The surface is the content, so the pool is the content's size — at the
// buffer's scale, which CommitFrame reconciles on the first frame.
bool App::CreateBuffers() {
  return pool_->Create(content_w_, content_h_, shm_.Get()->GetProxy());
}

// ── Callback implementations ────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
  need_redraw_ = true;
}

// The configure carries the size of the window geometry, which includes the
// decoration — and the frame is what declares that geometry, so the frame is
// what reads it back. The two are exact inverses, and splitting them is how a
// window ends up resizing itself by the decoration on every round trip.
//
// A zero axis means the compositor has no opinion and the size is ours to pick;
// that is how an un-maximize arrives, and the frame answers it from the size it
// tracks as the one to restore to.
void App::OnToplevelConfigure(int32_t width, int32_t height) {
  frame_.ContentSizeForConfigure(width, height, &content_w_, &content_h_);
  need_redraw_ = true;
}

// The compositor is the authority on these. Tracking them from our own button
// clicks instead would be optimistic: a maximize can be refused, and can
// equally arrive from a keybinding or a double-click we never saw. The frame
// styles itself from them, and needs maximized/fullscreen to know whether a
// configure's size is one to remember.
void App::OnToplevelStates(const wl::ToplevelStates& states) {
  frame_.SetToplevelStates(states.activated, states.maximized,
                           states.fullscreen);
}

void App::OnToplevelClose() {
  running_ = false;
}

// The compositor's preferred scale for this surface — which changes when the
// window is dragged onto an output with a different scale.
void App::OnPreferredScale(const int32_t scale_120) noexcept {
  // Clamp to a sane upper bound before it reaches an allocation. This value is
  // the compositor's, and it is multiplied by the surface size to get the
  // buffer size: a bug or a silly value at the far end would otherwise overflow
  // that product into a negative or wrapped dimension, which wl_shm would then
  // be asked to allocate. Same reasoning as the kMaxDim clamp on configure.
  // 8x is far past any real display and still leaves the product bounded.
  static constexpr int32_t kMaxScale120 = 8 * wl::ScalePolicy::kUnityScale120;

  // Honored only with a viewport to present the physical buffer at the
  // logical size; without one the window would come out the wrong size.
  if (!CanScale() || scale_120 <= 0 || scale_120 > kMaxScale120 ||
      scale_120 == scale_120_)
    return;
  scale_120_ = scale_120;
  frame_.SetScale(scale_120_);
  need_redraw_ = true;
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
//
// Every event goes to the frame, which takes the ones on its own surface and
// ignores the rest — so there is no test to make here. The gestures are its
// own: a button fires on release over the button it was pressed on, a title-bar
// press becomes a move only past the toolkit's drag threshold, and a second
// press within the toolkit's double-click time maximizes instead. Move, resize,
// maximize and minimize it drives itself, because it has the toplevel. Only
// close comes back, because only the application can decide to exit.

void App::OnPointerEnter(const wl::PointerEvent& ev) noexcept {
  enter_serial_ = ev.serial;  // set_cursor must carry an enter serial
  frame_.OnPointerEnter({ev.x, ev.y, ev.serial, ev.time, ev.surface});
  cursor_.Reset();  // the compositor resets the cursor on enter; re-apply
  UpdateCursor();
}

void App::OnPointerLeave() noexcept {
  frame_.OnPointerLeave();
}

void App::OnPointerMotion(const wl::PointerEvent& ev) noexcept {
  frame_.OnPointerMotion({ev.x, ev.y, ev.serial, ev.time, nullptr});
  UpdateCursor();
}

void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  frame_.OnPointerButton({ev.serial, ev.time, ev.button, ev.state});
  if (frame_.CloseRequested())
    running_ = false;
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
  // The surface is the content. The decoration is on a subsurface of its own
  // behind it, so it adds nothing here — no margins to carry, and no rect to
  // paint the content into but the whole buffer.
  //
  // Logical: what the window is worth on screen, and what the configure is
  // expressed in.
  const int cw = content_w_;
  const int ch = content_h_;
  // Physical: what the buffer actually holds. The viewport presents it back at
  // the logical size, so the two are derived from the same pair and cannot
  // drift apart.
  const wl::ScalePolicy::BufferSize buf =
      wl::ScalePolicy::ToBuffer(cw, ch, scale_120_);

  // The compositor may still be displaying buffers from the current pool, so a
  // resize retires it rather than freeing it, and a fresh pool takes over.
  if (pool_->width != buf.width || pool_->height != buf.height) {
    if (!retired_ || retired_->AllReleased())
      retired_ = std::move(pool_);  // Drops any older pool, now safely idle.
    pool_ = std::make_unique<BufferPool>();
    if (!pool_->Create(buf.width, buf.height, shm_.Get()->GetProxy())) {
      std::fprintf(stderr, "xdg-csd: buffer pool %dx%d failed\n", buf.width,
                   buf.height);
      return;
    }
  }

  // Free the retired pool as soon as its buffers come back.
  if (retired_ && retired_->AllReleased())
    retired_.reset();

  const int idx = pool_->NextFree();
  if (idx < 0) {
    // No free buffer: the compositor still holds every one. Skip the paint —
    // but commit anyway, because the frame callback requested for the next
    // frame is double-buffered state and only takes effect on a commit.
    // Returning without one leaves it unarmed, so no further frame callback
    // ever arrives and the render loop stops for good: the window freezes on
    // whatever was last displayed and never redraws again, however much the
    // compositor reconfigures it.
    surface_.Get()->Commit();
    return;
  }

  auto* pixels = static_cast<uint32_t*>(pool_->PixelData(idx));
  const std::size_t npixels = static_cast<std::size_t>(buf.width) *
                              static_cast<std::size_t>(buf.height);

  // Let the frame pump the plugin's own event source (theme changes, etc.)
  // before it is asked for pixels. This is the app's only chance: the event
  // loop blocks in poll() on the Wayland fd, so it is drained once per frame —
  // which is enough precisely because the demo animates and therefore always
  // has a next frame.
  frame_.Dispatch();

  // Redraw and commit the decoration, and with it the window geometry the
  // configure above is read back against — both are the frame's, and it is
  // Commit() that declares them. Its subsurface is synchronized, so none of it
  // reaches the screen until the content surface commits below: that is what
  // keeps the frame and the content in step through a resize, and it is why
  // this has to come first.
  frame_.Commit(cw, ch);

  // The content is the application's, so it is the application that scales it:
  // painted at physical size, filling the buffer.
  paint_content({pixels, npixels}, 0, 0, buf.width, buf.height, buf.width,
                time_ms);

  // Every pixel of this surface is content, and the content is opaque. What
  // rounds its corners and casts a shadow is the frame's surface, which is the
  // frame's to declare.
  UpdateOpaqueRegion(0, 0, cw, ch);

  // The viewport presents the physical buffer back at the logical size.
  // Without it the surface would be the buffer's size, so the window would come
  // out scaled up on screen and the window geometry would describe only the
  // fraction of it that the logical size covers — which is why a preferred
  // scale is only ever honored when there is a viewport to answer it with.
  //
  // Sent every frame it could change: the destination and the buffer are both
  // derived from (cw, ch, scale_120_), so they cannot disagree.
  if (CanScale() && (cw != viewport_w_ || ch != viewport_h_)) {
    viewport_.Get()->SetDestination(cw, ch);
    viewport_w_ = cw;
    viewport_h_ = ch;
  }

  surface_.Get()->Attach(
      pool_->bufs.at(static_cast<std::size_t>(idx)).Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, cw, ch);
  surface_.Get()->Commit();
  pool_->bufs.at(static_cast<std::size_t>(idx)).Get()->busy = true;
}

// ── MainLoop ────────────────────────────────────────────────────────────────

App::~App() {
  // Release keyboard + pointer (SeatManager sends the versioned releases)
  // before the WlPtr members are destroyed.
  seat_.Release();
  // The frame's own decoration object is torn down by frame_, which is declared
  // after the toplevel it borrows and so goes first.
  fractional_.Reset();
  viewport_.Reset();
}

bool App::MainLoop() {
  // The frame settled this in Init: it asked, and the compositor answered.
  // "Not drawn by this client" means only that — whether the compositor
  // decorates is its business, and with no decoration manager it was never
  // asked at all.
  const bool csd = frame_.DrawsClientSide();
  std::fprintf(stderr,
               "xdg-csd: %dx%d content, decorations=%s "
               "(press ESC%s to quit)\n",
               content_w_, content_h_,
               csd ? "client-side" : "not drawn by this client",
               csd ? " or click ✕" : "");

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

  // Which plugin is compiled in, whether it starts, and whether the compositor
  // is preferred to it are all settled behind wl::csd::MakeCsdPlugin(), which
  // the frame is handed in App::CreateWindow(). This example asks for a
  // decorated window and is told nothing about how that was arranged.
  App app{content_w, content_h, title};
  return app.Run();
}

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//           cppcoreguidelines-pro-bounds-constant-array-index,
//           cppcoreguidelines-pro-type-reinterpret-cast)
