// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// xdg-csd — Client-Side Decoration example with plugin architecture
//
// Demonstrates the zxdg_decoration_manager_v1 protocol for negotiating
// CSD vs SSD with the compositor, and renders client-side decorations
// (title bar, window buttons, resize borders) using a pluggable CSD
// rendering backend:
//   • GtkCsdPlugin      — decorations rendered by GTK's own theming (optional)
//   • CairoCsdPlugin    — Cairo + Pango decorations (optional)
//   • FallbackCsdPlugin — flat-color SHM decorations (always available)
//
// The csd option (WAYLAND_CXX_CSD under CMake) chooses between them, and
// defaults to ssd: no plugin at all, the compositor is asked to decorate, and
// the binary has no toolkit dependency.  csd=auto compiles the best available
// plugin but still prefers the compositor, using the plugin only if it
// declines; naming a plugin forces client-side.  A themed plugin may also
// decline at run time, in which case the fallback is used.
//
// Decoration features:
//   • Decoration mode negotiation via xdg-decoration-unstable-v1
//   • Title bar rendering with window control buttons
//   • Resize borders around the window
//   • Interactive move (click title bar), resize (click border),
//     and close (click close button) via pointer events
//   • xdg_surface.set_window_geometry declaring the window's visible bounds,
//     which here is the whole surface: every pixel of it is visible
//     decoration.  See OnToplevelConfigure() — the two must agree.
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
#include "xdg_decoration_unstable_v1_client.hpp"  // namespace xdg_decoration_unstable_v1::client
#include "xdg_shell_client.hpp"                   // namespace xdg_shell::client

// ── Framework headers ────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/csd_plugin.hpp>
#include <wl/cursor.hpp>
// Exactly one plugin is compiled in, chosen by the build (the csd option), and
// with csd=ssd there is none at all. csd_dep supplies both the define that says
// which, and the include path for the themed plugins' headers — those are not
// framework headers, because they need a .cpp and a toolkit that a header-only
// framework cannot carry.
//
// The fallback comes along with the GTK plugin regardless: it is the run-time
// landing spot when GTK declines to start.
#ifdef USE_GTK_CSD
#include <wl/csd_fallback.hpp>
#include "csd_gtk.hpp"
#elif defined(USE_CAIRO_CSD)
#include "csd_cairo.hpp"
#elif defined(USE_FALLBACK_CSD)
#include <wl/csd_fallback.hpp>
#endif
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/scale_policy.hpp>  // wl::ScalePolicy — buffer/viewport sizing
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
// CSD types — provided by the plugin interface in <wl/csd_plugin.hpp>
// ══════════════════════════════════════════════════════════════════════════════

using wl::csd::CsdPlugin;
using wl::csd::HitZone;

// Whether to ask the compositor to decorate even though a plugin is compiled
// in. Set by the build for csd=auto: a client should prefer the compositor's
// own decorations, which are cheaper and match the rest of the desktop, and
// only draw its own if the compositor declines. Naming a plugin explicitly
// (csd=gtk / cairo / fallback) leaves this off, forcing client-side — the point
// of naming one is to see it.
#ifdef CSD_PREFER_SSD
inline constexpr bool kPreferSsd = true;
#else
inline constexpr bool kPreferSsd = false;
#endif

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
  App(int content_w,
      int content_h,
      const char* title,
      std::unique_ptr<CsdPlugin> plugin)
      : content_w_(content_w),
        content_h_(content_h),
        title_(title),
        csd_plugin_(std::move(plugin)) {
    restore_w_ = content_w;
    restore_h_ = content_h;
    use_csd_ = csd_plugin_ != nullptr;
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
  void OnPreferredScale(int32_t scale_120) noexcept;
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

  // The size to return to when the compositor stops imposing one. Tracked
  // rather than remembered at the moment of maximizing, because an interactive
  // resize changes it too.
  int restore_w_ = 0;
  int restore_h_ = 0;
  bool fullscreen_ = false;

  // ── Decoration state ──────────────────────────────────────────────────────
  // Default when the compositor offers no decoration manager: draw our own
  // frame if we have a plugin, otherwise go undecorated and let the compositor
  // do whatever it does.
  bool use_csd_ = false;
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

  // ── Title-bar gesture ─────────────────────────────────────────────────────
  [[nodiscard]] bool IsDoubleClick(
      const wl::PointerButtonEvent& ev) const noexcept;
  void ToggleMaximized() noexcept;

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
  int pointer_x_ =
      -1;  // -1 ⇒ pointer not over the surface (wl::csd::InputState)
  int pointer_y_ = -1;
  bool pointer_pressed_ = false;
  // Which zone the press landed in, so the release can be matched to it: a
  // button only fires when press and release agree.
  HitZone pressed_zone_ = HitZone::None;

  // A press on the title bar, held until it turns out to be a drag, a
  // double-click, or neither.  The move cannot start on press: it grabs the
  // pointer, and the second click of a double-click would never arrive.
  bool title_press_pending_ = false;
  uint32_t title_press_serial_ = 0;
  uint32_t title_press_time_ = 0;  // 0 ⇒ no press to pair a double-click with
  int title_press_x_ = 0;
  int title_press_y_ = 0;
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

  // Last opaque region submitted, to avoid re-sending it every frame.
  int opaque_x_ = -1, opaque_y_ = -1, opaque_w_ = -1, opaque_h_ = -1;

  // Last window geometry submitted, likewise. Both are double-buffered state
  // that only needs re-declaring when it actually changes.
  int geometry_x_ = -1, geometry_y_ = -1, geometry_w_ = -1, geometry_h_ = -1;

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
    using namespace xdg_decoration_unstable_v1::client;
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
    } else if (iface == zxdg_decoration_manager_v1_traits::interface_name) {
      decoration_mgr_name_ = name;
      decoration_mgr_ver_ = ver;
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
    // Nothing to negotiate with, so the decision is ours alone: decorate if we
    // have a plugin, and otherwise leave it to whatever the compositor does
    // unasked — which may well be nothing.
    // Nothing to prefer and nothing to ask: if we have a plugin it is the only
    // way this window gets a frame, whatever csd=auto would rather have done.
    std::fprintf(stderr,
                 "xdg-csd: zxdg_decoration_manager_v1 not available — %s\n",
                 csd_plugin_ ? "drawing client-side decorations unasked"
                             : "no plugin either; the window will be "
                               "undecorated unless the compositor decorates "
                               "it");
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

  // Negotiate decoration mode via zxdg_decoration_manager_v1.
  if (!decoration_mgr_.IsNull()) {
    if (wl_proxy* raw = wl::construct<
            zxdg_toplevel_decoration_v1_traits,
            zxdg_decoration_manager_v1_traits::Op::GetToplevelDecoration>(
            *decoration_mgr_.Get(), xdg_toplevel_.Get()->GetProxy())) {
      if (wl::SetupHandler(decoration_, raw)) {
        decoration_.Get()->app_ = this;
        // Ask for client-side only when we both can and want to. With no
        // plugin there is nothing to draw a frame with, and under csd=auto the
        // compositor is preferred; either way the compositor has the last word
        // and answers with a configure.
        const bool want_csd = csd_plugin_ != nullptr && !kPreferSsd;
        decoration_.Get()->SetMode(static_cast<uint32_t>(
            want_csd ? ZxdgToplevelDecorationV1Mode::ClientSide
                     : ZxdgToplevelDecorationV1Mode::ServerSide));
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
  return pool_->Create(SurfaceWidth(), SurfaceHeight(), shm_.Get()->GetProxy());
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
  static constexpr int kMaxDim = 16384;

  if (width > 0 && height > 0) {
    // width/height are the size of the window geometry rectangle, so this must
    // stay the exact inverse of the SetWindowGeometry() call in CommitFrame().
    // That geometry is the whole surface, so the decoration comes off here to
    // get the content area.  If the two ever disagree the window resizes itself
    // by the difference on every configure.
    if (use_csd_ && csd_plugin_) {
      // Only the decoration that is part of the window: a shadow lies outside
      // the window geometry the compositor just sized, so subtracting it here
      // would shrink the content by the shadow on every configure.
      const wl::csd::Margins v = csd_plugin_->VisibleMargins();
      content_w_ = width - v.left - v.right;
      content_h_ = height - v.top - v.bottom;
    } else {
      content_w_ = width;
      content_h_ = height;
    }
  } else {
    // A zero dimension means the compositor has no opinion and the size is
    // ours to pick. It is how a compositor says "go back to whatever you
    // were", so this is the path an un-maximize takes -- and ignoring it left
    // the window holding its maximized buffer, still looking maximized while
    // the compositor believed it had been restored. The compositor then had
    // nothing to change on the next maximize, so it stayed silent and the
    // window was stuck for good.
    //
    // Only the zero axes are ours to choose; the other still binds.
    if (width <= 0)
      content_w_ = restore_w_;
    if (height <= 0)
      content_h_ = restore_h_;
  }

  content_w_ = std::clamp(content_w_, 1, kMaxDim);
  content_h_ = std::clamp(content_h_, 1, kMaxDim);

  // Remember the size to come back to, but only while the compositor is not
  // imposing one: a maximized or fullscreen size is the compositor's, not a
  // size we would ever choose to return to.
  if (!maximized_ && !fullscreen_) {
    restore_w_ = content_w_;
    restore_h_ = content_h_;
  }

  need_redraw_ = true;
}

// The compositor is the authority on both of these. Tracking them from our own
// button clicks instead would be optimistic: a maximize can be refused, and can
// equally arrive from a keybinding or a double-click we never saw.
void App::OnToplevelStates(const wl::ToplevelStates& states) {
  focused_ = states.activated;
  maximized_ = states.maximized;
  fullscreen_ = states.fullscreen;
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
  if (csd_plugin_)
    csd_plugin_->SetScale(scale_120_);
  need_redraw_ = true;
}

void App::OnDecorationConfigure(uint32_t mode) {
  const bool was_csd = use_csd_;
  // Without a plugin the answer is always server-side, whatever the compositor
  // asks for: there is nothing compiled in that could draw a frame.
  use_csd_ = csd_plugin_ != nullptr &&
             mode == static_cast<uint32_t>(
                         xdg_decoration_unstable_v1::client::
                             ZxdgToplevelDecorationV1Mode::ClientSide);
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
  pressed_zone_ = HitZone::None;  // A press the pointer leaves is canceled.
  title_press_pending_ = false;
  title_press_time_ = 0;
}

void App::OnPointerMotion(const wl::PointerEvent& ev) noexcept {
  pointer_x_ = static_cast<int>(ev.x);
  pointer_y_ = static_cast<int>(ev.y);
  UpdateCursor();

  // A held title-bar press becomes a move once the pointer travels far enough
  // to mean it. Below the threshold it stays a click, so a double-click still
  // has a chance to happen.
  if (!title_press_pending_)
    return;
  const int threshold = csd_plugin_ ? csd_plugin_->DragThreshold() : 8;
  if (std::abs(pointer_x_ - title_press_x_) <= threshold &&
      std::abs(pointer_y_ - title_press_y_) <= threshold)
    return;

  title_press_pending_ = false;
  title_press_time_ = 0;  // Became a drag: not half of a double-click.
  pointer_pressed_ = false;
  pressed_zone_ = HitZone::None;
  if (wl_proxy* const seat = seat_.Seat())
    xdg_toplevel_.Get()->Move(seat, title_press_serial_);
}

// A window button fires on release over the button it was pressed on — the
// convention every toolkit follows, and the only one that lets a press be
// visible as a pressed state or be taken back by releasing elsewhere.  Acting
// on press instead makes the pressed styling unreachable, since the window is
// already gone by the time it would be drawn.
//
// Move and resize are the exception: they are drags, so they start on press and
// the compositor takes a grab from there.
void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  if (ev.button != BTN_LEFT)
    return;

  const HitZone zone = HitTest(pointer_x_, pointer_y_);

  if (ev.state == WL_POINTER_BUTTON_STATE_PRESSED) {
    pointer_pressed_ = true;
    pressed_zone_ = zone;

    // The seat proxy backs interactive move/resize; the button serial
    // authorizes the grab (see wl::SeatManager::Seat()).
    wl_proxy* const seat = seat_.Seat();
    const uint32_t edge = wl::csd::HitZoneToResizeEdge(zone);

    if (zone == HitZone::TitleBar && seat != nullptr) {
      // Do not start the move yet.  xdg_toplevel.move() hands the pointer to
      // the compositor's grab, and every later event — including the second
      // click of a double-click — goes there instead of here.  So the press is
      // held: it becomes a move once the pointer travels past the drag
      // threshold (see OnPointerMotion), and a maximize toggle if a second
      // press arrives first.
      if (IsDoubleClick(ev)) {
        ToggleMaximized();
        title_press_time_ = 0;  // Consumed: a third click starts over.
        pointer_pressed_ = false;
        pressed_zone_ = HitZone::None;
        return;
      }
      title_press_pending_ = true;
      title_press_serial_ = ev.serial;
      title_press_time_ = ev.time;
      title_press_x_ = pointer_x_;
      title_press_y_ = pointer_y_;
      return;
    }

    if (edge != 0 && seat != nullptr) {
      xdg_toplevel_.Get()->Resize(seat, ev.serial, edge);
    } else {
      return;  // A button: hold the press and wait for the release.
    }

    // The compositor owns the pointer for the duration of the grab and the
    // matching release never arrives, so the press is finished with here.
    pointer_pressed_ = false;
    pressed_zone_ = HitZone::None;
    return;
  }

  // ── Release ───────────────────────────────────────────────────────────────
  const HitZone pressed = pressed_zone_;
  pointer_pressed_ = false;
  pressed_zone_ = HitZone::None;
  // A title-bar press that never traveled is just a click. title_press_time_
  // survives, so a second press soon after can still pair with it.
  title_press_pending_ = false;

  // Released somewhere other than where the press landed: canceled.
  if (pressed != zone)
    return;

  switch (zone) {
    case HitZone::CloseButton:
      running_ = false;
      break;

    case HitZone::MaximizeButton:
      ToggleMaximized();
      break;

    case HitZone::MinimizeButton:
      xdg_toplevel_.Get()->SetMinimized();
      break;

    default:
      break;
  }
}

// A second press on the title bar close enough in time and place to the last
// one. The interval and the slop are the toolkit's own settings, not invented
// here: both are desktop-wide user preferences.
bool App::IsDoubleClick(const wl::PointerButtonEvent& ev) const noexcept {
  if (title_press_time_ == 0)
    return false;
  const int interval = csd_plugin_ ? csd_plugin_->DoubleClickTimeMs() : 400;
  const int threshold = csd_plugin_ ? csd_plugin_->DragThreshold() : 8;
  return (ev.time - title_press_time_) <= static_cast<uint32_t>(interval) &&
         std::abs(pointer_x_ - title_press_x_) <= threshold &&
         std::abs(pointer_y_ - title_press_y_) <= threshold;
}

// Request only. maximized_ follows the configure the compositor sends back, so
// a refused request does not leave us drawing the wrong icon.
void App::ToggleMaximized() noexcept {
  if (maximized_)
    xdg_toplevel_.Get()->UnsetMaximized();
  else
    xdg_toplevel_.Get()->SetMaximized();
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
  // Logical: what the window is worth on screen, and what every margin, hit
  // test and configure is expressed in.
  const int sw = SurfaceWidth();
  const int sh = SurfaceHeight();
  // Physical: what the buffer actually holds. The viewport presents it back at
  // the logical size, so the two are derived from the same pair and cannot
  // drift apart.
  const wl::ScalePolicy::BufferSize buf =
      wl::ScalePolicy::ToBuffer(sw, sh, scale_120_);

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
    csd_plugin_->RenderDecoration(pixels, buf.width, sw, sh, content_w_,
                                  content_h_);
    // The content is the application's, so it is the application that scales
    // it: painted at physical size into the physical rect the logical content
    // rect maps to.
    paint_content(
        {pixels, npixels}, wl::ScalePolicy::ScaledDim(m.left, scale_120_),
        wl::ScalePolicy::ScaledDim(m.top, scale_120_),
        wl::ScalePolicy::ScaledDim(content_w_, scale_120_),
        wl::ScalePolicy::ScaledDim(content_h_, scale_120_), buf.width, time_ms);

    // Window geometry is the window's visible bounds: the surface inset by
    // whatever part of the decoration is not window, which is the shadow.
    //
    // It must stay the exact inverse of how OnToplevelConfigure reads the size
    // back — the compositor's configure carries the size of *this* rectangle.
    // If the two disagree the window resizes itself by the difference on every
    // configure, and a drag-resize sends them continuously.
    //
    // Sent only when it changes. It is double-buffered state, so re-sending it
    // on every frame is legal but means every commit re-declares the geometry —
    // and a compositor is entitled to treat that as the client having a say
    // about its size mid-negotiation.
    const wl::csd::Margins sm = csd_plugin_->ShadowMargins();
    const int gx = sm.left;
    const int gy = sm.top;
    const int gw = sw - sm.left - sm.right;
    const int gh = sh - sm.top - sm.bottom;
    if (gx != geometry_x_ || gy != geometry_y_ || gw != geometry_w_ ||
        gh != geometry_h_) {
      xdg_surface_.Get()->SetWindowGeometry(gx, gy, gw, gh);
      geometry_x_ = gx;
      geometry_y_ = gy;
      geometry_w_ = gw;
      geometry_h_ = gh;
    }

    // Only the content rect is guaranteed opaque. The title bar rounds its top
    // corners and a shadow is translucent by definition, so both are left to
    // blend: claiming they are opaque would stop the compositor blending them
    // and the rounding would come out as black corners.
    UpdateOpaqueRegion(m.left, m.top, content_w_, content_h_);
  } else {
    paint_content({pixels, npixels}, 0, 0, sw, sh, sw, time_ms);
    UpdateOpaqueRegion(0, 0, sw, sh);
  }

  // The viewport presents the physical buffer back at the logical size.
  // Without it the surface would be the buffer's size, so the window would come
  // out scaled up on screen and the window geometry would describe only the
  // fraction of it that the logical size covers — which is why a preferred
  // scale is only ever honored when there is a viewport to answer it with.
  //
  // Sent every frame it could change: the destination and the buffer are both
  // derived from (sw, sh, scale_120_), so they cannot disagree.
  if (CanScale() && (sw != viewport_w_ || sh != viewport_h_)) {
    viewport_.Get()->SetDestination(sw, sh);
    viewport_w_ = sw;
    viewport_h_ = sh;
  }

  surface_.Get()->Attach(
      pool_->bufs.at(static_cast<std::size_t>(idx)).Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, sw, sh);
  surface_.Get()->Commit();
  pool_->bufs.at(static_cast<std::size_t>(idx)).Get()->busy = true;
}

// ── MainLoop ────────────────────────────────────────────────────────────────

App::~App() {
  // Release keyboard + pointer (SeatManager sends the versioned releases)
  // before the WlPtr members are destroyed.
  seat_.Release();
  // Destroy decoration before toplevel (protocol requirement).
  decoration_.Reset();
  fractional_.Reset();
  viewport_.Reset();
}

bool App::MainLoop() {
  // "SSD" here means only that this client is not drawing them — whether the
  // compositor does is its business, and with no decoration manager it was
  // never asked.
  std::fprintf(stderr,
               "xdg-csd: %dx%d content, decorations=%s "
               "(press ESC%s to quit)\n",
               content_w_, content_h_,
               use_csd_ ? "client-side" : "not drawn by this client",
               use_csd_ ? " or click ✕" : "");

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
  // options), and csd=ssd compiles none: the example then decorates nothing
  // and asks the compositor to do it instead.
  //
  // The themed plugin can still fail at run time — GTK may be linked but have
  // no usable display or theme — so it is asked rather than assumed, and the
  // dependency-free fallback covers the refusal.
  std::unique_ptr<CsdPlugin> plugin;
#ifdef USE_GTK_CSD
  plugin = wl::csd::GtkCsdPlugin::TryCreate();
  if (plugin) {
    std::fprintf(stderr, "xdg-csd: using GTK CSD plugin\n");
  } else {
    plugin = std::make_unique<wl::csd::FallbackCsdPlugin>();
    std::fprintf(stderr,
                 "xdg-csd: GTK unavailable at run time — using fallback CSD "
                 "plugin\n");
  }
#elif defined(USE_CAIRO_CSD)
  plugin = std::make_unique<wl::csd::CairoCsdPlugin>();
  std::fprintf(stderr, "xdg-csd: using Cairo CSD plugin\n");
#elif defined(USE_FALLBACK_CSD)
  plugin = std::make_unique<wl::csd::FallbackCsdPlugin>();
  std::fprintf(stderr, "xdg-csd: using fallback CSD plugin\n");
#else
  std::fprintf(stderr,
               "xdg-csd: no CSD plugin compiled in — requesting server-side "
               "decorations\n");
#endif
  if (plugin && kPreferSsd) {
    std::fprintf(stderr,
                 "xdg-csd: preferring server-side decorations; the plugin is "
                 "only used if the compositor declines\n");
  }

  App app{content_w, content_h, title, std::move(plugin)};
  return app.Run();
}

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//           cppcoreguidelines-pro-bounds-constant-array-index,
//           cppcoreguidelines-pro-type-reinterpret-cast)
