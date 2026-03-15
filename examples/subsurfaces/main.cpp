// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// subsurfaces — C++23 port of Weston clients/subsurfaces.c
//
// Demonstrates wl_subsurface using wl_shm solid-colour surfaces:
//   • Main surface (green)          — XDG toplevel, default 400 × 300
//   • Red sub-surface (desync)      — animates position independently
//   • Blue sub-surface (sync)       — commits only with the parent
//
// The sub-surface layout mirrors Weston's original:
//   side = min(width, height) / 2
//   red:  (width−side, 0)         size side × (height−side)
//   blue: (width−side, height−side) size side × side
//
// Controls:
//   Space  — toggle red sub-surface animation
//   Up     — shrink window (height −100, min 150)
//   Down   — grow window  (height +100, max 600)
//   Escape — quit

// ── Generated C++ protocol headers ───────────────────────────────────────────
// wayland_client.hpp  → namespace wayland::client  (from wayland.xml)
// xdg_shell_client.hpp → namespace xdg_shell::client (from xdg-shell.xml)
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── System Wayland C headers
// ──────────────────────────────────────────────────
extern "C" {
// Provides extern wl_*_interface symbols used by wl_iface() below.
#include <wayland-client-protocol.h>
// KEY_ESC, KEY_SPACE, KEY_UP, KEY_DOWN and WL_KEYBOARD_KEY_STATE_PRESSED.
#include <linux/input-event-codes.h>
// memfd_create, mmap, munmap, ftruncate
#include <sys/mman.h>
#include <unistd.h>  // close()
}

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>  // std::data

// ── POSIX
// ─────────────────────────────────────────────────────────────────────
#include <poll.h>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// <wayland-client-protocol.h> exposes extern const wl_interface symbols for
// every core Wayland interface.  We simply forward to them.
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
const wl_interface& wl_seat_traits::wl_iface() noexcept {
  return wl_seat_interface;
}
const wl_interface& wl_keyboard_traits::wl_iface() noexcept {
  return wl_keyboard_interface;
}
const wl_interface& wl_subcompositor_traits::wl_iface() noexcept {
  return wl_subcompositor_interface;
}
const wl_interface& wl_subsurface_traits::wl_iface() noexcept {
  return wl_subsurface_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// xdg-shell wl_interface definitions
//
// There is no pre-built system symbol for xdg-shell (unlike core Wayland).
// We reproduce the tables the C wayland-scanner generates from xdg-shell.xml
// (version 7) so that libwayland can type-check and dispatch correctly.
// ══════════════════════════════════════════════════════════════════════════════

// Forward declarations — the types array references all five interfaces before
// any of them are fully defined.
extern const wl_interface xdg_wm_base_iface_def;
extern const wl_interface xdg_positioner_iface_def;
extern const wl_interface xdg_surface_iface_def;
extern const wl_interface xdg_toplevel_iface_def;
extern const wl_interface xdg_popup_iface_def;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* xdg_shell_types[] = {
    nullptr,                    // [0]  scalar / no-type slots
    nullptr,                    // [1]
    nullptr,                    // [2]
    nullptr,                    // [3]
    &xdg_positioner_iface_def,  // [4]  create_positioner → new_id
    &xdg_surface_iface_def,     // [5]  get_xdg_surface   → new_id
    &wl_surface_interface,      // [6]  get_xdg_surface   → surface object
    &xdg_toplevel_iface_def,    // [7]  get_toplevel      → new_id
    &xdg_popup_iface_def,       // [8]  get_popup         → new_id
    &xdg_surface_iface_def,     // [9]  get_popup         → parent (?o)
    &xdg_positioner_iface_def,  // [10] get_popup         → positioner
    &xdg_toplevel_iface_def,    // [11] set_parent        → ?o
    &wl_seat_interface,         // [12] show_window_menu  → seat
    nullptr,                    // [13] show_window_menu  → serial
    nullptr,                    // [14] show_window_menu  → x
    nullptr,                    // [15] show_window_menu  → y
    &wl_seat_interface,         // [16] move              → seat
    nullptr,                    // [17] move              → serial
    &wl_seat_interface,         // [18] resize            → seat
    nullptr,                    // [19] resize            → serial
    nullptr,                    // [20] resize            → edges
    &wl_output_interface,       // [21] set_fullscreen    → output (?o)
    &wl_seat_interface,         // [22] grab              → seat
    nullptr,                    // [23] grab              → serial
    &xdg_positioner_iface_def,  // [24] reposition        → positioner
    nullptr,                    // [25] reposition        → token
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static constexpr const wl_interface** kScalarTypes = &xdg_shell_types[0];

static constexpr wl_message xdg_wm_base_requests[] = {
    {"destroy", "", nullptr},
    {"create_positioner", "n", &xdg_shell_types[4]},
    {"get_xdg_surface", "no", &xdg_shell_types[5]},
    {"pong", "u", kScalarTypes},
};
static constexpr wl_message xdg_wm_base_events[] = {
    {"ping", "u", kScalarTypes},
};

static constexpr wl_message xdg_positioner_requests[] = {
    {"destroy", "", nullptr},
    {"set_size", "ii", kScalarTypes},
    {"set_anchor_rect", "iiii", kScalarTypes},
    {"set_anchor", "u", kScalarTypes},
    {"set_gravity", "u", kScalarTypes},
    {"set_constraint_adjustment", "u", kScalarTypes},
    {"set_offset", "ii", kScalarTypes},
    {"set_reactive", "3", nullptr},
    {"set_parent_size", "3ii", kScalarTypes},
    {"set_parent_configure", "3u", kScalarTypes},
};

static constexpr wl_message xdg_surface_requests[] = {
    {"destroy", "", nullptr},
    {"get_toplevel", "n", &xdg_shell_types[7]},
    {"get_popup", "n?oo", &xdg_shell_types[8]},
    {"set_window_geometry", "iiii", kScalarTypes},
    {"ack_configure", "u", kScalarTypes},
};
static constexpr wl_message xdg_surface_events[] = {
    {"configure", "u", kScalarTypes},
};

static constexpr wl_message xdg_toplevel_requests[] = {
    {"destroy", "", nullptr},
    {"set_parent", "?o", &xdg_shell_types[11]},
    {"set_title", "s", kScalarTypes},
    {"set_app_id", "s", kScalarTypes},
    {"show_window_menu", "ouii", &xdg_shell_types[12]},
    {"move", "ou", &xdg_shell_types[16]},
    {"resize", "ouu", &xdg_shell_types[18]},
    {"set_max_size", "ii", kScalarTypes},
    {"set_min_size", "ii", kScalarTypes},
    {"set_maximized", "", nullptr},
    {"unset_maximized", "", nullptr},
    {"set_fullscreen", "?o", &xdg_shell_types[21]},
    {"unset_fullscreen", "", nullptr},
    {"set_minimized", "", nullptr},
};
static constexpr wl_message xdg_toplevel_events[] = {
    {"configure", "iia", kScalarTypes},
    {"close", "", nullptr},
    {"configure_bounds", "4ii", kScalarTypes},
    {"wm_capabilities", "5a", kScalarTypes},
};

static constexpr wl_message xdg_popup_requests[] = {
    {"destroy", "", nullptr},
    {"grab", "ou", &xdg_shell_types[22]},
    {"reposition", "3ou", &xdg_shell_types[24]},
};
static constexpr wl_message xdg_popup_events[] = {
    {"configure", "iiii", kScalarTypes},
    {"popup_done", "", nullptr},
    {"repositioned", "3u", kScalarTypes},
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// clang-format off
const wl_interface xdg_wm_base_iface_def = {
    "xdg_wm_base",    7,
    4,  std::data(xdg_wm_base_requests),    1, std::data(xdg_wm_base_events)};
const wl_interface xdg_positioner_iface_def = {
    "xdg_positioner", 7,
    10, std::data(xdg_positioner_requests), 0, nullptr};
const wl_interface xdg_surface_iface_def = {
    "xdg_surface",    7,
    5,  std::data(xdg_surface_requests),    1, std::data(xdg_surface_events)};
const wl_interface xdg_toplevel_iface_def = {
    "xdg_toplevel",   7,
    14, std::data(xdg_toplevel_requests),   4, std::data(xdg_toplevel_events)};
const wl_interface xdg_popup_iface_def = {
    "xdg_popup",      7,
    3,  std::data(xdg_popup_requests),      3, std::data(xdg_popup_events)};
// clang-format on

namespace xdg_shell::client {

const wl_interface& xdg_wm_base_traits::wl_iface() noexcept {
  return xdg_wm_base_iface_def;
}
const wl_interface& xdg_positioner_traits::wl_iface() noexcept {
  return xdg_positioner_iface_def;
}
const wl_interface& xdg_surface_traits::wl_iface() noexcept {
  return xdg_surface_iface_def;
}
const wl_interface& xdg_toplevel_traits::wl_iface() noexcept {
  return xdg_toplevel_iface_def;
}
const wl_interface& xdg_popup_traits::wl_iface() noexcept {
  return xdg_popup_iface_def;
}

}  // namespace xdg_shell::client

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

  [[nodiscard]] bool Create(std::size_t n) noexcept {
    fd = memfd_create("subsurfaces-shm", 0);
    if (fd < 0) {
      std::fprintf(stderr, "subsurfaces: memfd_create: %s\n",
                   std::strerror(errno));
      return false;
    }
    if (ftruncate(fd, static_cast<off_t>(n)) < 0) {
      std::fprintf(stderr, "subsurfaces: ftruncate: %s\n",
                   std::strerror(errno));
      return false;
    }
    data = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
      std::fprintf(stderr, "subsurfaces: mmap: %s\n", std::strerror(errno));
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
  void OnFormat(uint32_t fmt) override {
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

// ── WlSeatHandler
// ─────────────────────────────────────────────────────────────

class WlSeatHandler : public wayland::client::CWlSeat<WlSeatHandler> {
 public:
  App* app_ = nullptr;
  void OnCapabilities(uint32_t caps) override;
  void OnName(const char* /*name*/) override {}
};

// ── WlKeyboardHandler
// ─────────────────────────────────────────────────────────

class WlKeyboardHandler
    : public wayland::client::CWlKeyboard<WlKeyboardHandler> {
 public:
  App* app_ = nullptr;
  void OnKeymap(uint32_t /*fmt*/, int32_t fd, uint32_t /*sz*/) override {
    close(fd);
  }
  void OnEnter(uint32_t /*serial*/,
               wl_proxy* /*surface*/,
               wl_array* /*keys*/) override {}
  void OnLeave(uint32_t /*serial*/, wl_proxy* /*surface*/) override {}
  void OnKey(uint32_t /*serial*/,
             uint32_t /*time*/,
             uint32_t key,
             uint32_t state) override;
  void OnModifiers(uint32_t /*serial*/,
                   uint32_t /*mods_depressed*/,
                   uint32_t /*mods_latched*/,
                   uint32_t /*mods_locked*/,
                   uint32_t /*group*/) override {}
  void OnRepeatInfo(int32_t /*rate*/, int32_t /*delay*/) override {}
};

// ── XdgWmBaseHandler
// ──────────────────────────────────────────────────────────

class XdgWmBaseHandler
    : public xdg_shell::client::CXdgWmBase<XdgWmBaseHandler> {
 public:
  void OnPing(uint32_t serial) override { Pong(serial); }
};

// ── XdgSurfaceHandler
// ─────────────────────────────────────────────────────────

class XdgSurfaceHandler
    : public xdg_shell::client::CXdgSurface<XdgSurfaceHandler> {
 public:
  App* app_ = nullptr;
  void OnConfigure(uint32_t serial) override;
};

// ── XdgToplevelHandler
// ────────────────────────────────────────────────────────

class XdgToplevelHandler
    : public xdg_shell::client::CXdgToplevel<XdgToplevelHandler> {
 public:
  App* app_ = nullptr;
  void OnConfigure(int32_t w, int32_t h, wl_array* states) override;
  void OnClose() override;
  void OnConfigureBounds(int32_t /*w*/, int32_t /*h*/) override {}
  void OnWmCapabilities(wl_array* /*caps*/) override {}
};

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
  void OnSeatCapabilities(uint32_t caps);
  void OnKey(uint32_t key, uint32_t state);
  /// Called by WlCallbackHandler::OnDone — advance the animation.
  void OnFrameReady(uint32_t time_ms) noexcept;

 private:
  // ── Member declaration order = reverse destruction order ───────────────
  //
  // Destruction sequence:
  //   frame_cb_ → keyboard_ → seat_
  //   → xdg_toplevel_ → xdg_surface_ → xdg_wm_base_
  //   → blue_subsurface_ → blue_surface_
  //   → red_subsurface_  → red_surface_
  //   → main_surface_
  //   → subcompositor_ → compositor_ → shm_
  //   → shm_mem_ (munmap + close)
  //   → registry_ → display_

  struct DisplayRaii {
    wl_display* d = nullptr;
    ~DisplayRaii() noexcept {
      if (d)
        wl_display_disconnect(d);
    }
  } display_;

  wl::CRegistry registry_;

  // ── Global bindings (event-less: use Attach, not _SetProxy) ───────────
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<WlSubcompositorHandler> subcompositor_;

  // ── XDG shell ──────────────────────────────────────────────────────────
  wl::WlPtr<XdgWmBaseHandler> xdg_wm_base_;

  // ── Input ──────────────────────────────────────────────────────────────
  wl::WlPtr<WlSeatHandler> seat_;
  wl::WlPtr<WlKeyboardHandler> keyboard_;

  // ── SHM memory (declared before buffers and surfaces so it outlives
  //    everything that holds pointers into it) ────────────────────────────
  ShmMapping shm_mem_;

  // ── Buffers (hold pointers into shm_mem_.data) ────────────────────────
  wl::WlPtr<WlBufferHandler> main_buf_;
  wl::WlPtr<WlBufferHandler> red_buf_;
  wl::WlPtr<WlBufferHandler> blue_buf_;

  // ── Surfaces ───────────────────────────────────────────────────────────
  wl::WlPtr<WlSurfaceHandler> main_surface_;
  wl::WlPtr<XdgSurfaceHandler> xdg_surface_;
  wl::WlPtr<XdgToplevelHandler> xdg_toplevel_;

  wl::WlPtr<WlSurfaceHandler> red_surface_;
  wl::WlPtr<WlSubsurfaceHandler> red_subsurface_;

  wl::WlPtr<WlSurfaceHandler> blue_surface_;
  wl::WlPtr<WlSubsurfaceHandler> blue_subsurface_;

  // ── Frame-pacing callback (on the red sub-surface for desync animation)
  wl::WlPtr<WlCallbackHandler> frame_cb_;

  // ── Application state ─────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  bool animate_ = true;

  int width_ = 400;
  int height_ = 300;

  // Derived geometry (recalculated in ApplyGeometry).
  int side_ = 0;    // sub-surface side dimension
  int red_x_ = 0;   // base x of red sub-surface
  int red_y_ = 0;   // base y of red sub-surface
  int red_w_ = 0;   // width of red sub-surface
  int red_h_ = 0;   // height of red sub-surface
  int blue_x_ = 0;  // base x of blue sub-surface
  int blue_y_ = 0;  // base y of blue sub-surface
  int blue_w_ = 0;  // width of blue sub-surface
  int blue_h_ = 0;  // height of blue sub-surface

  // Animation phase (driven by frame timestamps).
  double anim_phase_ = 0.0;

  // ── Globals recorded during registry scan ─────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t subcompositor_name_ = 0, subcompositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t seat_name_ = 0, seat_ver_ = 0;

  static constexpr int kRoundtripTimeoutMs = 5000;

  // ── Pipeline steps ─────────────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool CreateBuffers();
  bool InitialCommit();
  bool MainLoop();

  [[nodiscard]] bool RoundtripWithTimeout(
      int timeout_ms = kRoundtripTimeoutMs) const noexcept;

  void RequestFrameCallback() noexcept;
  void AdvanceAnimation(uint32_t time_ms) noexcept;

  /// Compute sub-surface geometry from current width_/height_.
  void ApplyGeometry() noexcept;

  /// (Re)create all SHM buffers to match the current geometry.
  /// Returns false on allocation failure.
  [[nodiscard]] bool ReallocBuffers() noexcept;

  // ── Input teardown ─────────────────────────────────────────────────────
  void ReleaseKeyboard() noexcept;
  void ReleaseSeat() noexcept;

  // ── Helper: bind a registry global and install the CRTP event listener ─
  template <typename Traits, typename Handler>
  [[nodiscard]] bool BindHandler(wl::WlPtr<Handler>& ptr,
                                 uint32_t name,
                                 uint32_t ver) noexcept {
    wl_proxy* raw =
        registry_.Bind<Traits>(name, std::min(ver, Traits::version));
    if (!raw)
      return false;
    // Set the back-pointer only when the handler exposes one (C++23 requires).
    if constexpr (requires { ptr.Get()->app_; })
      ptr.Get()->app_ = this;
    ptr.Get()->_SetProxy(raw);
    return true;
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler method implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void XdgSurfaceHandler::OnConfigure(uint32_t serial) {
  AckConfigure(serial);
  app_->OnXdgSurfaceConfigure(serial);
}

void XdgToplevelHandler::OnConfigure(int32_t w,
                                     int32_t h,
                                     wl_array* /*states*/) {
  app_->OnToplevelConfigure(w, h);
}

void XdgToplevelHandler::OnClose() {
  app_->OnToplevelClose();
}

void WlSeatHandler::OnCapabilities(uint32_t caps) {
  app_->OnSeatCapabilities(caps);
}

void WlKeyboardHandler::OnKey(uint32_t /*serial*/,
                              uint32_t /*time*/,
                              uint32_t key,
                              uint32_t state) {
  app_->OnKey(key, state);
}

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  app_->OnFrameReady(time_ms);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  ReleaseKeyboard();
  ReleaseSeat();
  // RAII handles all protocol object teardown in reverse declaration order.
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
  if (!InitialCommit())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  display_.d = wl_display_connect(nullptr);
  if (!display_.d) {
    std::fprintf(stderr, "subsurfaces: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── RoundtripWithTimeout
// ──────────────────────────────────────────────────────

bool App::RoundtripWithTimeout(int timeout_ms) const noexcept {
  bool sync_done = false;

  wl_callback* const sync_cb = wl_display_sync(display_.d);
  if (!sync_cb)
    return false;

  const auto guard = wl::ScopeExit{[sync_cb] { wl_callback_destroy(sync_cb); }};

  static constexpr wl_callback_listener kSyncListener = {
      [](void* data, wl_callback* /*cb*/, uint32_t /*serial*/) noexcept {
        *static_cast<bool*>(data) = true;
      }};
  wl_callback_add_listener(sync_cb, &kSyncListener, &sync_done);

  const int fd = wl_display_get_fd(display_.d);
  bool ok = true;

  while (!sync_done && ok) {
    if (wl_display_flush(display_.d) < 0) {
      if (errno != EAGAIN) {
        ok = false;
        break;
      }
      pollfd out{fd, POLLOUT, 0};
      if (poll(&out, 1, timeout_ms) <= 0) {
        ok = false;
        break;
      }
      continue;
    }
    pollfd in{fd, POLLIN, 0};
    const int n = poll(&in, 1, timeout_ms);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    if (wl_display_dispatch(display_.d) < 0) {
      ok = false;
      break;
    }
  }

  return ok && sync_done;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.d)) {
    std::fprintf(stderr, "subsurfaces: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                            std::string_view iface, uint32_t ver) {
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
      seat_name_ = name;
      seat_ver_ = ver;
    }
  });

  if (!RoundtripWithTimeout()) {
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
  if (!BindHandler<wl_shm_traits>(shm_, shm_name_, shm_ver_)) {
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
  if (!BindHandler<xdg_wm_base_traits>(xdg_wm_base_, xdg_wm_base_name_,
                                       xdg_wm_base_ver_)) {
    std::fprintf(stderr, "subsurfaces: xdg_wm_base bind failed\n");
    return false;
  }

  // wl_seat — optional.
  if (seat_name_)
    std::ignore = BindHandler<wl_seat_traits>(seat_, seat_name_, seat_ver_);

  // Second roundtrip so that wl_shm format events arrive.
  if (!RoundtripWithTimeout()) {
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
  //   red:  (width−side, 0)           size side × (height−side)
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
  if (wl_proxy* raw = wl::construct<xdg_surface_traits,
                                    xdg_wm_base_traits::Op::GetXdgSurface>(
          *xdg_wm_base_.Get(), main_surface_.Get()->GetProxy())) {
    xdg_surface_.Get()->app_ = this;
    xdg_surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "subsurfaces: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }

  // ── xdg_toplevel ─────────────────────────────────────────────────────────
  if (wl_proxy* raw = wl::construct<xdg_toplevel_traits,
                                    xdg_surface_traits::Op::GetToplevel>(
          *xdg_surface_.Get())) {
    xdg_toplevel_.Get()->app_ = this;
    xdg_toplevel_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "subsurfaces: xdg_surface.get_toplevel failed\n");
    return false;
  }

  xdg_toplevel_.Get()->SetTitle("Wayland Sub-surface Demo");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.subsurfaces");
  // Lock the initial size so the compositor doesn't free-size us on startup.
  xdg_toplevel_.Get()->SetMinSize(100, 100);

  // ── Red sub-surface ───────────────────────────────────────────────────────
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
  // Desynchronized: the red sub-surface commits independently.
  red_subsurface_.Get()->SetDesync();

  // ── Blue sub-surface ──────────────────────────────────────────────────────
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
  // Synchronized: the blue sub-surface state is applied on the parent commit.
  blue_subsurface_.Get()->SetSync();

  // XDG shell requires an initial empty commit on the main surface so the
  // compositor sends the mandatory xdg_surface::configure event.  Only after
  // that event is received (and auto-acked by XdgSurfaceHandler::OnConfigure)
  // may the client attach a buffer and commit again.  This mirrors the pattern
  // in examples/simple-egl/main.cpp:CreateSurfaces().
  main_surface_.Get()->Commit();
  if (!RoundtripWithTimeout()) {
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

  // Destroy previous buffers if any.
  main_buf_.Reset();
  red_buf_.Reset();
  blue_buf_.Reset();
  shm_mem_.Reset();

  const std::size_t main_stride = static_cast<std::size_t>(width_) * 4u;
  const std::size_t main_size = main_stride * static_cast<std::size_t>(height_);
  const std::size_t red_stride = static_cast<std::size_t>(red_w_) * 4u;
  const std::size_t red_size = red_stride * static_cast<std::size_t>(red_h_);
  const std::size_t blue_stride = static_cast<std::size_t>(blue_w_) * 4u;
  const std::size_t blue_size = blue_stride * static_cast<std::size_t>(blue_h_);

  const std::size_t total = main_size + red_size + blue_size;

  if (!shm_mem_.Create(total)) {
    std::fprintf(stderr, "subsurfaces: SHM allocation failed\n");
    return false;
  }

  // Paint solid colours.
  auto* pixels = static_cast<uint32_t*>(shm_mem_.data);
  // Green: XRGB  (R=0, G=0xCC, B=0)
  std::fill(pixels, pixels + (main_size / 4u), 0x0000CC00u);
  // Red: XRGB
  auto* red_px = pixels + (main_size / 4u);
  std::fill(red_px, red_px + (red_size / 4u), 0x00CC0000u);
  // Blue: XRGB
  auto* blue_px = red_px + (red_size / 4u);
  std::fill(blue_px, blue_px + (blue_size / 4u), 0x000000CCu);

  // Create a single pool covering all surfaces.
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

  // Red buffer.
  if (wl_proxy* raw =
          wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
              *pool.Get(), static_cast<int32_t>(main_size),
              static_cast<int32_t>(red_w_), static_cast<int32_t>(red_h_),
              static_cast<int32_t>(red_stride), WL_SHM_FORMAT_XRGB8888)) {
    red_buf_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "subsurfaces: wl_shm_pool.create_buffer (red) failed\n");
    return false;
  }

  // Blue buffer.
  if (wl_proxy* raw =
          wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
              *pool.Get(), static_cast<int32_t>(main_size + red_size),
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
  // Position the sub-surfaces.
  red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_),
                                     static_cast<int32_t>(red_y_));
  blue_subsurface_.Get()->SetPosition(static_cast<int32_t>(blue_x_),
                                      static_cast<int32_t>(blue_y_));

  // Commit the sub-surfaces first.
  red_surface_.Get()->Attach(red_buf_.Get()->GetProxy(), 0, 0);
  red_surface_.Get()->Damage(0, 0, red_w_, red_h_);
  red_surface_.Get()->Commit();
  red_buf_.Get()->busy = true;

  blue_surface_.Get()->Attach(blue_buf_.Get()->GetProxy(), 0, 0);
  blue_surface_.Get()->Damage(0, 0, blue_w_, blue_h_);
  blue_surface_.Get()->Commit();
  blue_buf_.Get()->busy = true;

  // Commit the main surface (this also applies blue's sync'd position).
  main_surface_.Get()->Attach(main_buf_.Get()->GetProxy(), 0, 0);
  main_surface_.Get()->Damage(0, 0, width_, height_);
  main_surface_.Get()->Commit();
  main_buf_.Get()->busy = true;

  // Arm the first frame callback on the red surface to drive the animation.
  // The callback is only delivered to the compositor on the next commit of
  // the surface it was registered on, so commit red_surface_ after arming.
  RequestFrameCallback();
  red_surface_.Get()->Commit();

  return true;
}

// ── Frame-callback helpers
// ────────────────────────────────────────────────────

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw =
          wl::construct<wl_c, wl_s::Op::Frame>(*red_surface_.Get())) {
    frame_cb_.Get()->app_ = this;
    frame_cb_.Get()->_SetProxy(raw);
  }
}

void App::OnFrameReady(uint32_t time_ms) noexcept {
  // Release the spent callback proxy.
  wl_proxy* const spent = frame_cb_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent)
      wl_proxy_destroy(spent);
  }};

  if (animate_)
    AdvanceAnimation(time_ms);

  // Arm the next frame callback BEFORE the commit so both the callback
  // request and the surface state are delivered to the compositor in the
  // same message batch (mirrors examples/simple-egl/main.cpp:OnFrameReady).
  // The commit also delivers the position change set by AdvanceAnimation
  // (which takes effect on the child commit in desync mode).
  RequestFrameCallback();
  red_surface_.Get()->Commit();
}

void App::AdvanceAnimation(uint32_t time_ms) noexcept {
  // Oscillate the red sub-surface horizontally within its column.
  // Amplitude = side_ / 3, period = 2 s.
  constexpr double kPeriodMs = 2000.0;
  anim_phase_ = static_cast<double>(time_ms) * (2.0 * M_PI / kPeriodMs);

  const auto amplitude = static_cast<double>(side_) / 3.0;
  const auto dx = static_cast<int32_t>(amplitude * std::sin(anim_phase_));

  red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_) + dx,
                                     static_cast<int32_t>(red_y_));
  // The commit that delivers the new position (desync mode) and the
  // next frame callback happens in OnFrameReady, after RequestFrameCallback.
}

// ── App callbacks
// ─────────────────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(int32_t w, int32_t h) {
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    width_ = static_cast<int>(std::min(w, kMaxDim));
    height_ = static_cast<int>(std::min(h, kMaxDim));
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnSeatCapabilities(uint32_t caps) {
  using wl_s = wayland::client::wl_seat_traits;
  using wl_k = wayland::client::wl_keyboard_traits;

  if (const bool has_kbd = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0u;
      has_kbd && keyboard_.IsNull()) {
    if (wl_proxy* raw =
            wl::construct<wl_k, wl_s::Op::GetKeyboard>(*seat_.Get())) {
      keyboard_.Get()->app_ = this;
      keyboard_.Get()->_SetProxy(raw);
    }
  } else if (!has_kbd && !keyboard_.IsNull()) {
    ReleaseKeyboard();
  }
}

void App::OnKey(uint32_t key, uint32_t state) {
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
      const int new_h = std::max(150, height_ - 100);
      if (new_h != height_) {
        height_ = new_h;
        ApplyGeometry();
        if (ReallocBuffers()) {
          // Recommit everything with new dimensions.
          red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_),
                                             static_cast<int32_t>(red_y_));
          blue_subsurface_.Get()->SetPosition(static_cast<int32_t>(blue_x_),
                                              static_cast<int32_t>(blue_y_));
          red_surface_.Get()->Attach(red_buf_.Get()->GetProxy(), 0, 0);
          red_surface_.Get()->Damage(0, 0, red_w_, red_h_);
          red_surface_.Get()->Commit();
          blue_surface_.Get()->Attach(blue_buf_.Get()->GetProxy(), 0, 0);
          blue_surface_.Get()->Damage(0, 0, blue_w_, blue_h_);
          blue_surface_.Get()->Commit();
          main_surface_.Get()->Attach(main_buf_.Get()->GetProxy(), 0, 0);
          main_surface_.Get()->Damage(0, 0, width_, height_);
          main_surface_.Get()->Commit();
          xdg_surface_.Get()->SetWindowGeometry(0, 0, width_, height_);
        }
      }
      break;
    }

    case KEY_DOWN: {
      const int new_h = std::min(600, height_ + 100);
      if (new_h != height_) {
        height_ = new_h;
        ApplyGeometry();
        if (ReallocBuffers()) {
          red_subsurface_.Get()->SetPosition(static_cast<int32_t>(red_x_),
                                             static_cast<int32_t>(red_y_));
          blue_subsurface_.Get()->SetPosition(static_cast<int32_t>(blue_x_),
                                              static_cast<int32_t>(blue_y_));
          red_surface_.Get()->Attach(red_buf_.Get()->GetProxy(), 0, 0);
          red_surface_.Get()->Damage(0, 0, red_w_, red_h_);
          red_surface_.Get()->Commit();
          blue_surface_.Get()->Attach(blue_buf_.Get()->GetProxy(), 0, 0);
          blue_surface_.Get()->Damage(0, 0, blue_w_, blue_h_);
          blue_surface_.Get()->Commit();
          main_surface_.Get()->Attach(main_buf_.Get()->GetProxy(), 0, 0);
          main_surface_.Get()->Damage(0, 0, width_, height_);
          main_surface_.Get()->Commit();
          xdg_surface_.Get()->SetWindowGeometry(0, 0, width_, height_);
        }
      }
      break;
    }

    default:
      break;
  }
}

// ── Input teardown
// ────────────────────────────────────────────────────────────

void App::ReleaseKeyboard() noexcept {
  if (keyboard_.IsNull())
    return;
  using Kbd = wayland::client::wl_keyboard_traits;
  if (seat_ver_ >= Kbd::Op::Since::Release)
    keyboard_.Get()->Release();
  keyboard_.Reset();
}

void App::ReleaseSeat() noexcept {
  if (seat_.IsNull())
    return;
  using S = wayland::client::wl_seat_traits;
  if (seat_ver_ >= S::Op::Since::Release)
    seat_.Get()->Release();
  seat_.Reset();
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

static void LogWlError(wl_display* display, const char* context) noexcept {
  const int err = wl_display_get_error(display);
  const int code = err ? err : errno;
  if (code == EPROTO) {
    const wl_interface* iface = nullptr;
    uint32_t obj_id = 0;
    const uint32_t proto_code =
        wl_display_get_protocol_error(display, &iface, &obj_id);
    std::fprintf(stderr,
                 "subsurfaces: compositor protocol error (%s): code %u"
                 " on %s object %u\n",
                 context, proto_code, iface ? iface->name : "unknown", obj_id);
  } else {
    std::fprintf(stderr, "subsurfaces: compositor disconnected (%s): %s\n",
                 context, std::strerror(code));
  }
}

bool App::MainLoop() {
  std::printf("subsurfaces: entering event loop\n");
  std::printf("  Space  — toggle red sub-surface animation\n");
  std::printf("  Up/Down — resize window\n");
  std::printf("  Escape — quit\n");

  const int fd = wl_display_get_fd(display_.d);

  while (running_) {
    while (wl_display_flush(display_.d) < 0) {
      if (errno != EAGAIN) {
        LogWlError(display_.d, "flush");
        return false;
      }
      pollfd pfd{fd, POLLOUT, 0};
      if (poll(&pfd, 1, -1) < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
    }

    if (wl_display_dispatch_pending(display_.d) < 0) {
      LogWlError(display_.d, "dispatch_pending");
      return false;
    }

    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, -1) < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }

    if (wl_display_dispatch(display_.d) < 0) {
      LogWlError(display_.d, "dispatch");
      return false;
    }
  }

  std::printf("subsurfaces: exiting cleanly\n");
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  App app;
  return app.Run();
}
