// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// presentation-shm — C++23 port of Weston clients/presentation-shm.c
//
// Demonstrates the wp_presentation protocol for accurate frame-timing
// feedback using wl_shm for an animated spinning-wheel pattern.
//
// Three run modes (selected by command-line flag):
//   -f  feedback      (default) wl_surface.frame drives commits; prints
//                               f2c, c2p, f2p, p2p, t2p per frame.
//   -i  feedback-idle           same but sleeps 1 s between frames.
//   -p  low-lat present         wp_presentation_feedback drives commits for
//                               minimum latency; prints c2p, p2p, t2p.
//
// Additional options:
//   -d MSECS   emulate rendering cost by sleeping MSECS before each commit.
//
// Usage:
//   presentation_shm [-f|-i|-p] [-d MSECS]

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "presentation_time_client.hpp"  // namespace presentation_time::client
#include "wayland_client.hpp"            // namespace wayland::client
#include "xdg_shell_client.hpp"          // namespace xdg_shell::client

// ── System Wayland C headers ──────────────────────────────────────────────────
extern "C" {
#include <wayland-client-protocol.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
}

// ── Framework headers ─────────────────────────────────────────────────────────
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

// ── Standard library ──────────────────────────────────────────────────────────
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <string_view>

// ── POSIX ─────────────────────────────────────────────────────────────────────
#include <poll.h>

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
// xdg-shell wl_interface definitions (no pre-built system symbols)
// ══════════════════════════════════════════════════════════════════════════════

extern const wl_interface xdg_wm_base_iface_def;
extern const wl_interface xdg_positioner_iface_def;
extern const wl_interface xdg_surface_iface_def;
extern const wl_interface xdg_toplevel_iface_def;
extern const wl_interface xdg_popup_iface_def;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* xdg_shell_types[] = {
    nullptr,                    // [0]  scalar
    nullptr,                    // [1]
    nullptr,                    // [2]
    nullptr,                    // [3]
    &xdg_positioner_iface_def,  // [4]  create_positioner
    &xdg_surface_iface_def,     // [5]  get_xdg_surface → new_id
    &wl_surface_interface,      // [6]  get_xdg_surface → surface
    &xdg_toplevel_iface_def,    // [7]  get_toplevel
    &xdg_popup_iface_def,       // [8]  get_popup → new_id
    &xdg_surface_iface_def,     // [9]  get_popup → parent
    &xdg_positioner_iface_def,  // [10] get_popup → positioner
    &xdg_toplevel_iface_def,    // [11] set_parent
    &wl_seat_interface,         // [12] show_window_menu → seat
    nullptr,                    // [13]
    nullptr,                    // [14]
    nullptr,                    // [15]
    &wl_seat_interface,         // [16] move
    nullptr,                    // [17]
    &wl_seat_interface,         // [18] resize
    nullptr,                    // [19]
    nullptr,                    // [20]
    &wl_output_interface,       // [21] set_fullscreen
    &wl_seat_interface,         // [22] grab
    nullptr,                    // [23]
    &xdg_positioner_iface_def,  // [24] reposition
    nullptr,                    // [25]
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static constexpr const wl_interface** kScalarTypes = &xdg_shell_types[0];

static constexpr wl_message xdg_wm_base_requests[] = {
    {"destroy", "", nullptr},
    {"create_positioner", "n", &xdg_shell_types[4]},
    {"get_xdg_surface", "no", &xdg_shell_types[5]},
    {"pong", "u", kScalarTypes},
};
static constexpr wl_message xdg_wm_base_events[] = {{"ping", "u", kScalarTypes}};

static constexpr wl_message xdg_positioner_requests[] = {
    {"destroy", "", nullptr},       {"set_size", "ii", kScalarTypes},
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
static constexpr wl_message xdg_surface_events[] = {{"configure", "u", kScalarTypes}};

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
// wp_presentation / wp_presentation_feedback wl_interface definitions
// ══════════════════════════════════════════════════════════════════════════════

extern const wl_interface wp_presentation_iface_def;
extern const wl_interface wp_presentation_feedback_iface_def;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* presentation_time_types[] = {
    nullptr,                              // [0] scalar
    &wl_surface_interface,               // [1] feedback → surface arg
    &wp_presentation_feedback_iface_def, // [2] feedback → callback new_id
    &wl_output_interface,                // [3] sync_output → output
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
static constexpr wl_message wp_presentation_requests[] = {
    {"destroy",  "",   nullptr},
    {"feedback", "on", &presentation_time_types[1]},
};
static constexpr wl_message wp_presentation_events[] = {
    {"clock_id", "u", &presentation_time_types[0]},
};
static constexpr wl_message wp_presentation_feedback_events[] = {
    {"sync_output", "o",        &presentation_time_types[3]},
    {"presented",   "uuuuuuu",  &presentation_time_types[0]},
    {"discarded",   "",         nullptr},
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)

// clang-format off
const wl_interface wp_presentation_iface_def = {
    "wp_presentation",          2,
    2, std::data(wp_presentation_requests),          1, std::data(wp_presentation_events)};
const wl_interface wp_presentation_feedback_iface_def = {
    "wp_presentation_feedback", 2,
    0, nullptr,                                      3, std::data(wp_presentation_feedback_events)};
// clang-format on

namespace presentation_time::client {
const wl_interface& wp_presentation_traits::wl_iface() noexcept {
  return wp_presentation_iface_def;
}
const wl_interface& wp_presentation_feedback_traits::wl_iface() noexcept {
  return wp_presentation_feedback_iface_def;
}
}  // namespace presentation_time::client

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
    fd = memfd_create("presentation-shm", 0);
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
// Run mode
// ══════════════════════════════════════════════════════════════════════════════

enum class RunMode { Feedback, FeedbackIdle, LowLatPresent };

static constexpr std::string_view run_mode_name(RunMode m) noexcept {
  switch (m) {
    case RunMode::Feedback:
      return "feedback";
    case RunMode::FeedbackIdle:
      return "feedback-idle";
    case RunMode::LowLatPresent:
      return "low-lat present";
  }
  return "?";
}

// ══════════════════════════════════════════════════════════════════════════════
// Timing utilities
// ══════════════════════════════════════════════════════════════════════════════

static constexpr int64_t kNsecPerSec = 1'000'000'000LL;

/// Combine the protocol's split tv_sec_hi / tv_sec_lo into a timespec.
static void timespec_from_proto(timespec& ts, uint32_t sec_hi, uint32_t sec_lo,
                                uint32_t nsec) noexcept {
  ts.tv_sec = (static_cast<int64_t>(sec_hi) << 32) |
              static_cast<int64_t>(sec_lo);
  ts.tv_nsec = static_cast<long>(nsec);
}

static uint32_t timespec_to_ms(const timespec& ts) noexcept {
  return static_cast<uint32_t>(ts.tv_sec) * 1000u +
         static_cast<uint32_t>(ts.tv_nsec / 1'000'000L);
}

static int64_t timespec_diff_us(const timespec& a,
                                const timespec& b) noexcept {
  return (a.tv_sec - b.tv_sec) * 1'000'000LL +
         (a.tv_nsec - b.tv_nsec) / 1000LL;
}

// ══════════════════════════════════════════════════════════════════════════════
// Pixel painting — identical to the Weston original
// ══════════════════════════════════════════════════════════════════════════════

/// Paint an animated spinning colour wheel into @p image (XRGB8888).
/// @p phase drives the rotation; call with increasing values for animation.
static void paint_pixels(void* image, int width, int height,
                         uint32_t phase) noexcept {
  const int halfh = height / 2;
  const int halfw = width / 2;
  auto* pixel = static_cast<uint32_t*>(image);

  const double ang = M_PI * 2.0 / 1'000'000.0 * static_cast<double>(phase);
  const double s = std::sin(ang);
  const double c = std::cos(ang);

  // Squared outer-radius threshold.
  int outer_r = (halfw < halfh ? halfw : halfh) - 16;
  outer_r *= outer_r;

  for (int y = 0; y < height; ++y) {
    const int oy = y - halfh;
    const int y2 = oy * oy;

    for (int x = 0; x < width; ++x) {
      const int ox = x - halfw;

      if (ox * ox + y2 > outer_r) {
        *pixel++ = (ox * oy > 0) ? 0xFF000000u : 0xFFFFFFFFu;
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

      *pixel++ = v;
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

class WlShmPoolHandler
    : public wayland::client::CWlShmPool<WlShmPoolHandler> {
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

// ── XdgWmBaseHandler ──────────────────────────────────────────────────────────

class XdgWmBaseHandler
    : public xdg_shell::client::CXdgWmBase<XdgWmBaseHandler> {
 public:
  void OnPing(uint32_t serial) override { Pong(serial); }
};

// ── XdgSurfaceHandler ─────────────────────────────────────────────────────────

class XdgSurfaceHandler
    : public xdg_shell::client::CXdgSurface<XdgSurfaceHandler> {
 public:
  App* app_ = nullptr;
  void OnConfigure(uint32_t serial) override;
};

// ── XdgToplevelHandler ────────────────────────────────────────────────────────

class XdgToplevelHandler
    : public xdg_shell::client::CXdgToplevel<XdgToplevelHandler> {
 public:
  App* app_ = nullptr;
  void OnConfigure(int32_t /*w*/, int32_t /*h*/,
                   wl_array* /*states*/) override {}
  void OnClose() override;
  void OnConfigureBounds(int32_t /*w*/, int32_t /*h*/) override {}
  void OnWmCapabilities(wl_array* /*caps*/) override {}
};

// ── WpPresentationHandler ─────────────────────────────────────────────────────

class WpPresentationHandler
    : public presentation_time::client::CWpPresentation<WpPresentationHandler> {
 public:
  clockid_t clk_id = CLOCK_MONOTONIC;
  void OnClockId(uint32_t id) override {
    clk_id = static_cast<clockid_t>(id);
  }
};

// ── WpPresentationFeedbackHandler ─────────────────────────────────────────────
//
// One instance is allocated per submitted frame and freed on presented /
// discarded.  The App is notified via virtual callbacks.

class WpPresentationFeedbackHandler
    : public presentation_time::client::CWpPresentationFeedback<
          WpPresentationFeedbackHandler> {
 public:
  App* app_ = nullptr;
  unsigned frame_no = 0;
  timespec commit{};
  timespec target{};
  uint32_t frame_stamp = 0;  // wl_callback timestamp at the time of commit

  void OnSyncOutput(wl_proxy* /*output*/) override {}
  void OnPresented(uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
                   uint32_t refresh_ns, uint32_t seq_hi, uint32_t seq_lo,
                   uint32_t flags) override;
  void OnDiscarded() override;
};

// ══════════════════════════════════════════════════════════════════════════════
// Buffer pool
//
// Pre-allocates kNumBuffers wl_shm buffers from a single SHM pool and
// provides next-available access with busy-tracking.
// ══════════════════════════════════════════════════════════════════════════════

static constexpr int kNumBuffers = 4;

struct BufferPool {
  ShmMapping mem;
  wl::WlPtr<WlBufferHandler> bufs[kNumBuffers];
  int next = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] bool Create(int w, int h, wl_proxy* shm_proxy) noexcept;

  // Returns the mapped pixel data for buffer index i.
  [[nodiscard]] void* PixelData(int i) const noexcept {
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    return static_cast<uint8_t*>(mem.data) +
           static_cast<std::size_t>(i) * stride *
               static_cast<std::size_t>(height);
  }

  // Finds and returns the next non-busy buffer index, or -1 if all are busy.
  [[nodiscard]] int NextFree() noexcept {
    for (int attempt = 0; attempt < kNumBuffers; ++attempt) {
      const int idx = (next + attempt) % kNumBuffers;
      if (!bufs[idx].Get()->busy) {
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
    std::fprintf(stderr, "presentation-shm: SHM allocation failed\n");
    return false;
  }

  // Create pool.
  // We build a temporary CProxyImpl wrapper around the raw shm proxy.
  // Since WlShmHandler (which owns the shm proxy) is stored in the App,
  // we receive the raw proxy and marshal via _MarshalNew directly.
  // Use the WlShmPoolHandler via wl::construct.
  wl::WlPtr<WlShmPoolHandler> pool;
  {
    // shm_raw is a wl_proxy* pointing to the wl_shm object.
    // We need to call wl_proxy_marshal_constructor on it to create the pool.
    // Build a temporary non-owning CProxyImpl around the raw shm proxy.
    // The cleanest approach: use the C API directly here.
    wl_shm_pool* raw_pool =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        wl_shm_create_pool(reinterpret_cast<wl_shm*>(shm_raw),
                           mem.fd, static_cast<int>(total));
    if (!raw_pool) {
      std::fprintf(stderr,
                   "presentation-shm: wl_shm_create_pool failed\n");
      return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    pool.Attach(reinterpret_cast<wl_proxy*>(raw_pool));
  }

  for (int i = 0; i < kNumBuffers; ++i) {
    const int32_t offset = static_cast<int32_t>(static_cast<std::size_t>(i) * per_buf);
    if (wl_proxy* raw =
            wl::construct<wl_buffer_traits,
                          wl_shm_pool_traits::Op::CreateBuffer>(
                *pool.Get(), offset, w, h, static_cast<int32_t>(stride),
                WL_SHM_FORMAT_XRGB8888)) {
      bufs[i].Get()->_SetProxy(raw);
    } else {
      std::fprintf(stderr,
                   "presentation-shm: wl_shm_pool.create_buffer [%d] "
                   "failed\n",
                   i);
      return false;
    }
  }

  pool.Reset();  // pool only needed during buffer creation
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  App(RunMode mode, int commit_delay_ms)
      : mode_(mode), commit_delay_ms_(commit_delay_ms) {}
  ~App();

  int Run();

  // ── Callbacks from CRTP handlers ────────────────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelClose();
  void OnFrameDone(uint32_t stamp_ms) noexcept;
  void OnFeedbackPresented(WpPresentationFeedbackHandler& fb,
                           uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                           uint32_t tv_nsec, uint32_t refresh_ns,
                           uint32_t seq_hi, uint32_t seq_lo,
                           uint32_t flags) noexcept;
  void OnFeedbackDiscarded(WpPresentationFeedbackHandler& fb) noexcept;

  /// Called by FeedkickHandler to update the display refresh period estimate.
  void UpdateRefresh(uint32_t refresh_ns) noexcept { refresh_nsec_ = refresh_ns; }

 private:
  // ── Configuration ────────────────────────────────────────────────────────
  RunMode mode_;
  int commit_delay_ms_ = 0;

  static constexpr int kWidth = 250;
  static constexpr int kHeight = 250;
  static constexpr int kRoundtripTimeoutMs = 5000;

  // ── Wayland objects (destruction order = reverse declaration) ────────────
  struct DisplayRaii {
    wl_display* d = nullptr;
    ~DisplayRaii() noexcept {
      if (d)
        wl_display_disconnect(d);
    }
  } display_;

  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<WpPresentationHandler> presentation_;
  wl::WlPtr<XdgWmBaseHandler> xdg_wm_base_;

  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<XdgSurfaceHandler> xdg_surface_;
  wl::WlPtr<XdgToplevelHandler> xdg_toplevel_;

  // Frame-pacing callback (feedback + feedback-idle modes).
  wl::WlPtr<WlCallbackHandler> frame_cb_;

  BufferPool pool_;

  // ── Application state ────────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  bool have_presentation_ = false;
  unsigned frame_seq_ = 0;         // monotone frame counter
  uint32_t refresh_nsec_ = 16'666'667u;  // 60 Hz default until feedback

  // Pending presentation-feedback objects (ownership transferred to the list).
  std::list<WpPresentationFeedbackHandler*> feedback_list_;
  // Last-presented feedback record (for p2p timing).
  WpPresentationFeedbackHandler* last_presented_ = nullptr;

  // ── Global IDs from registry scan ────────────────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t presentation_name_ = 0, presentation_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  // ── Pipeline ─────────────────────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateWindow();
  bool PreRender();
  bool MainLoop();
  void StartFeedbackMode();
  void StartPresentMode();

  [[nodiscard]] bool RoundtripWithTimeout(
      int timeout_ms = kRoundtripTimeoutMs) const noexcept;

  // ── Commit helpers ────────────────────────────────────────────────────────

  /// Apply optional rendering delay (emulates GPU work).
  void EmulateRendering() const noexcept;

  /// Create a wp_presentation_feedback for the current surface commit.
  void AttachPresentationFeedback(uint32_t stamp_ms) noexcept;

  /// Submit the next buffer to the compositor, acking any pending configure.
  void CommitNext(uint32_t stamp_ms) noexcept;

  /// Request a wl_surface.frame callback (feedback/feedback-idle modes).
  void RequestFrameCallback() noexcept;

  /// Kick the first commit in RUN_MODE_PRESENT.
  void Feedkick() noexcept;

  template <typename Traits, typename Handler>
  [[nodiscard]] bool BindHandler(wl::WlPtr<Handler>& ptr, uint32_t name,
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

  // Pending configure serial (0 = none pending).
  uint32_t configure_serial_ = 0;

  static void LogWlError(wl_display* display, const char* ctx) noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  app_->OnFrameDone(time_ms);
}

void XdgSurfaceHandler::OnConfigure(uint32_t serial) {
  AckConfigure(serial);
  app_->OnXdgSurfaceConfigure(serial);
}

void XdgToplevelHandler::OnClose() {
  app_->OnToplevelClose();
}

void WpPresentationFeedbackHandler::OnPresented(uint32_t tv_sec_hi,
                                                uint32_t tv_sec_lo,
                                                uint32_t tv_nsec,
                                                uint32_t refresh_ns,
                                                uint32_t seq_hi,
                                                uint32_t seq_lo,
                                                uint32_t flags) {
  app_->OnFeedbackPresented(*this, tv_sec_hi, tv_sec_lo, tv_nsec, refresh_ns,
                            seq_hi, seq_lo, flags);
}

void WpPresentationFeedbackHandler::OnDiscarded() {
  app_->OnFeedbackDiscarded(*this);
}

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  // Clean up pending feedback objects.
  for (auto* fb : feedback_list_)
    delete fb;
  delete last_presented_;
}

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  if (!CreateWindow())
    return EXIT_FAILURE;
  if (!PreRender())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  display_.d = wl_display_connect(nullptr);
  if (!display_.d) {
    std::fprintf(stderr, "presentation-shm: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── RoundtripWithTimeout ──────────────────────────────────────────────────────

bool App::RoundtripWithTimeout(int timeout_ms) const noexcept {
  bool sync_done = false;
  wl_callback* const sync_cb = wl_display_sync(display_.d);
  if (!sync_cb)
    return false;
  const auto guard = wl::ScopeExit{[sync_cb] { wl_callback_destroy(sync_cb); }};
  static constexpr wl_callback_listener kSyncListener = {
      [](void* data, wl_callback*, uint32_t) noexcept {
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

// ── ScanGlobals ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.d)) {
    std::fprintf(stderr, "presentation-shm: registry creation failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry&, uint32_t name,
                             std::string_view iface, uint32_t ver) {
    using namespace wayland::client;
    using namespace xdg_shell::client;
    using namespace presentation_time::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == wp_presentation_traits::interface_name) {
      presentation_name_ = name;
      presentation_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    }
  });

  if (!RoundtripWithTimeout()) {
    std::fprintf(stderr, "presentation-shm: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_ || !shm_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "presentation-shm: required globals not found\n");
    return false;
  }
  return true;
}

// ── BindGlobals ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;
  using namespace presentation_time::client;

  // wl_compositor — no events.
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "presentation-shm: wl_compositor bind failed\n");
    return false;
  }

  // wl_shm.
  if (!BindHandler<wl_shm_traits>(shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "presentation-shm: wl_shm bind failed\n");
    return false;
  }

  // xdg_wm_base.
  if (!BindHandler<xdg_wm_base_traits>(xdg_wm_base_, xdg_wm_base_name_,
                                        xdg_wm_base_ver_)) {
    std::fprintf(stderr, "presentation-shm: xdg_wm_base bind failed\n");
    return false;
  }

  // wp_presentation — optional.
  if (presentation_name_) {
    if (BindHandler<wp_presentation_traits>(presentation_, presentation_name_,
                                             presentation_ver_)) {
      have_presentation_ = true;
    }
  }
  if (!have_presentation_) {
    std::fprintf(stderr,
                 "presentation-shm: wp_presentation not available — "
                 "timing feedback disabled\n");
  }

  // Roundtrip so wl_shm.format and wp_presentation.clock_id arrive.
  if (!RoundtripWithTimeout()) {
    std::fprintf(stderr, "presentation-shm: timed out waiting for formats\n");
    return false;
  }

  constexpr uint32_t kXrgb8888 = 1u;
  if (!(shm_.Get()->formats & (1u << kXrgb8888))) {
    std::fprintf(stderr,
                 "presentation-shm: WL_SHM_FORMAT_XRGB8888 not supported\n");
    return false;
  }
  return true;
}

// ── CreateWindow ──────────────────────────────────────────────────────────────

bool App::CreateWindow() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_surface.
  if (wl_proxy* raw =
          wl::construct<wl_surface_traits,
                        wl_compositor_traits::Op::CreateSurface>(
              *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "presentation-shm: wl_compositor.create_surface failed\n");
    return false;
  }

  // xdg_surface.
  if (wl_proxy* raw =
          wl::construct<xdg_surface_traits,
                        xdg_wm_base_traits::Op::GetXdgSurface>(
              *xdg_wm_base_.Get(), surface_.Get()->GetProxy())) {
    xdg_surface_.Get()->app_ = this;
    xdg_surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "presentation-shm: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }

  // xdg_toplevel.
  if (wl_proxy* raw =
          wl::construct<xdg_toplevel_traits,
                        xdg_surface_traits::Op::GetToplevel>(
              *xdg_surface_.Get())) {
    xdg_toplevel_.Get()->app_ = this;
    xdg_toplevel_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "presentation-shm: xdg_surface.get_toplevel failed\n");
    return false;
  }

  // Format title like the original.
  char title[128];
  std::snprintf(title, sizeof(title), "presentation-shm: %.*s [delay %d ms]",
                static_cast<int>(run_mode_name(mode_).size()),
                run_mode_name(mode_).data(), commit_delay_ms_);
  xdg_toplevel_.Get()->SetTitle(title);
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.presentation-shm");
  xdg_toplevel_.Get()->SetMinSize(kWidth, kHeight);
  xdg_toplevel_.Get()->SetMaxSize(kWidth, kHeight);

  // Commit to trigger the configure sequence.
  surface_.Get()->Commit();
  if (!RoundtripWithTimeout()) {
    std::fprintf(stderr,
                 "presentation-shm: timed out waiting for configure\n");
    return false;
  }

  return true;
}

// ── PreRender ─────────────────────────────────────────────────────────────────

bool App::PreRender() {
  // Create the buffer pool.
  if (!pool_.Create(kWidth, kHeight, shm_.Get()->GetProxy()))
    return false;

  // Pre-paint all buffers at evenly-spaced phases (like the original).
  const int timefactor = 1'000'000 / kNumBuffers;
  for (int i = 0; i < kNumBuffers; ++i) {
    paint_pixels(pool_.PixelData(i), kWidth, kHeight,
                 static_cast<uint32_t>(i * timefactor));
  }

  return true;
}

// ── Commit helpers ────────────────────────────────────────────────────────────

void App::EmulateRendering() const noexcept {
  if (commit_delay_ms_ <= 0)
    return;
  timespec delay{.tv_sec = commit_delay_ms_ / 1000,
                 .tv_nsec = (commit_delay_ms_ % 1000) * 1'000'000L};
  nanosleep(&delay, nullptr);
}

void App::AttachPresentationFeedback(uint32_t stamp_ms) noexcept {
  if (!have_presentation_)
    return;

  using namespace presentation_time::client;

  auto* fb = new WpPresentationFeedbackHandler();
  fb->app_ = this;
  fb->frame_no = ++frame_seq_;
  fb->frame_stamp = stamp_ms;
  clock_gettime(presentation_.Get()->clk_id, &fb->commit);
  fb->target = fb->commit;

  // wp_presentation.feedback(surface) → new wp_presentation_feedback.
  if (wl_proxy* raw =
          wl::construct<wp_presentation_feedback_traits,
                        wp_presentation_traits::Op::Feedback>(
              *presentation_.Get(), surface_.Get()->GetProxy())) {
    fb->_SetProxy(raw);
    feedback_list_.push_back(fb);
  } else {
    delete fb;
  }
}

void App::CommitNext(uint32_t stamp_ms) noexcept {
  const int idx = pool_.NextFree();
  if (idx < 0) {
    std::fprintf(stderr, "presentation-shm: all buffers busy — skipping frame\n");
    return;
  }

  if (configure_serial_) {
    xdg_surface_.Get()->AckConfigure(configure_serial_);
    configure_serial_ = 0;
  }

  surface_.Get()->Attach(pool_.bufs[idx].Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, kWidth, kHeight);
  surface_.Get()->Commit();
  pool_.bufs[idx].Get()->busy = true;
}

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_cb_.Get()->app_ = this;
    frame_cb_.Get()->_SetProxy(raw);
  }
}

// ── Feedback callback implementations ────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t serial) {
  configure_serial_ = serial;
  configured_ = true;
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnFrameDone(uint32_t stamp_ms) noexcept {
  // Destroy the spent callback proxy before arming the next one.
  wl_proxy* const spent = frame_cb_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent)
      wl_proxy_destroy(spent);
  }};

  if (mode_ == RunMode::FeedbackIdle)
    sleep(1);

  EmulateRendering();

  RequestFrameCallback();
  AttachPresentationFeedback(stamp_ms);
  CommitNext(stamp_ms);
}

void App::OnFeedbackPresented(WpPresentationFeedbackHandler& fb,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec, uint32_t refresh_ns,
                               uint32_t seq_hi, uint32_t seq_lo,
                               uint32_t flags) noexcept {
  timespec present{};
  timespec_from_proto(present, tv_sec_hi, tv_sec_lo, tv_nsec);
  refresh_nsec_ = refresh_ns;

  const uint64_t seq =
      (static_cast<uint64_t>(seq_hi) << 32) | static_cast<uint64_t>(seq_lo);

  // Timing computations (all in ms / µs like the original).
  const uint32_t commit_ms = timespec_to_ms(fb.commit);
  const uint32_t present_ms = timespec_to_ms(present);
  const uint32_t f2c = commit_ms - fb.frame_stamp;
  const uint32_t c2p = present_ms - commit_ms;
  const uint32_t f2p = present_ms - fb.frame_stamp;

  const timespec* prev_present =
      last_presented_ ? &last_presented_->commit : &present;
  // We store 'present' in commit field for p2p tracking below.
  const int64_t p2p = timespec_diff_us(present, *prev_present);
  const int64_t t2p = timespec_diff_us(present, fb.target);

  // Build flag string: s=vsync c=hw_clock e=hw_completion z=zero_copy.
  char flagstr[8] = "____";
  if (flags & 0x1u)
    flagstr[0] = 's';
  if (flags & 0x2u)
    flagstr[1] = 'c';
  if (flags & 0x4u)
    flagstr[2] = 'e';
  if (flags & 0x8u)
    flagstr[3] = 'z';

  switch (mode_) {
    case RunMode::LowLatPresent:
      std::printf(
          "%6u: c2p %4u ms, p2p %5" PRId64 " us, t2p %6" PRId64
          " us, [%s] seq %" PRIu64 "\n",
          fb.frame_no, c2p, p2p, t2p, flagstr, seq);
      break;
    case RunMode::Feedback:
    case RunMode::FeedbackIdle:
      std::printf(
          "%6u: f2c %2u ms, c2p %2u ms, f2p %2u ms, p2p %5" PRId64
          " us, t2p %6" PRId64 " us, [%s] seq %" PRIu64 "\n",
          fb.frame_no, f2c, c2p, f2p, p2p, t2p, flagstr, seq);
      break;
  }
  std::fflush(stdout);

  // Store 'present' timestamp in commit field so p2p works next round.
  fb.commit = present;

  // Remove from list, transfer to last_presented_.
  feedback_list_.remove(&fb);
  delete last_presented_;
  last_presented_ = &fb;

  // For low-latency mode, kick the next frame immediately.
  if (mode_ == RunMode::LowLatPresent) {
    EmulateRendering();
    AttachPresentationFeedback(0);
    Feedkick();
    CommitNext(0);
  }
}

void App::OnFeedbackDiscarded(WpPresentationFeedbackHandler& fb) noexcept {
  std::printf("discarded %u\n", fb.frame_no);
  feedback_list_.remove(&fb);
  delete &fb;

  if (mode_ == RunMode::LowLatPresent) {
    EmulateRendering();
    AttachPresentationFeedback(0);
    Feedkick();
    CommitNext(0);
  }
}

// ── Feedkick (low-latency mode) ────────────────────────────────────────────────
//
// In RUN_MODE_PRESENT, instead of using wl_surface.frame we wait for the
// wp_presentation_feedback.presented event of a "kick" feedback object, then
// immediately commit the next frame.  This gives us the minimum possible
// latency while still pacing to the display.

class FeedkickHandler
    : public presentation_time::client::CWpPresentationFeedback<FeedkickHandler> {
 public:
  App* app_ = nullptr;
  void OnSyncOutput(wl_proxy* /*out*/) override {}
  void OnPresented(uint32_t, uint32_t, uint32_t, uint32_t refresh_ns, uint32_t,
                   uint32_t, uint32_t) override {
    // Update refresh estimate and trigger the next low-latency commit.
    app_->UpdateRefresh(refresh_ns);
    wl_proxy_destroy(Detach());
    delete this;
  }
  void OnDiscarded() override {
    wl_proxy_destroy(Detach());
    delete this;
  }
};

void App::Feedkick() noexcept {
  if (!have_presentation_)
    return;
  using namespace presentation_time::client;

  auto* fk = new FeedkickHandler();
  fk->app_ = this;
  if (wl_proxy* raw =
          wl::construct<wp_presentation_feedback_traits,
                        wp_presentation_traits::Op::Feedback>(
              *presentation_.Get(), surface_.Get()->GetProxy())) {
    fk->_SetProxy(raw);
  } else {
    delete fk;
  }
}

// ── MainLoop ──────────────────────────────────────────────────────────────────

void App::StartFeedbackMode() {
  // Kickstart: request the first frame callback, then commit.
  RequestFrameCallback();
  AttachPresentationFeedback(0);
  CommitNext(0);
}

void App::StartPresentMode() {
  EmulateRendering();
  AttachPresentationFeedback(0);
  Feedkick();
  CommitNext(0);
}

void App::LogWlError(wl_display* display, const char* ctx) noexcept {
  const int err = wl_display_get_error(display);
  const int code = err ? err : errno;
  if (code == EPROTO) {
    const wl_interface* iface = nullptr;
    uint32_t obj_id = 0;
    const uint32_t proto_code =
        wl_display_get_protocol_error(display, &iface, &obj_id);
    std::fprintf(stderr,
                 "presentation-shm: compositor protocol error (%s): "
                 "code %u on %s object %u\n",
                 ctx, proto_code, iface ? iface->name : "unknown", obj_id);
  } else {
    std::fprintf(stderr,
                 "presentation-shm: compositor disconnected (%s): %s\n", ctx,
                 std::strerror(code));
  }
}

bool App::MainLoop() {
  std::printf("presentation-shm: mode=%.*s delay=%d ms (press Ctrl-C to quit)\n",
              static_cast<int>(run_mode_name(mode_).size()),
              run_mode_name(mode_).data(), commit_delay_ms_);

  switch (mode_) {
    case RunMode::Feedback:
    case RunMode::FeedbackIdle:
      StartFeedbackMode();
      break;
    case RunMode::LowLatPresent:
      StartPresentMode();
      break;
  }

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

  std::fprintf(stderr, "presentation-shm exiting\n");
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

static volatile std::sig_atomic_t g_running = 1;

static void signal_handler(int /*sig*/) noexcept {
  g_running = 0;
}

static void print_usage(const char* prog) {
  std::fprintf(stderr,
               "Usage: %s [mode] [options]\n"
               "where 'mode' is one of\n"
               "  -f  feedback (default)\n"
               "  -i  feedback-idle (sleep 1s between frames)\n"
               "  -p  low-latency present mode\n"
               "and 'options' may include\n"
               "  -d MSECS  emulate rendering cost with a sleep\n",
               prog);
}

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);

  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);

  RunMode mode = RunMode::Feedback;
  int commit_delay_ms = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "-f")
      mode = RunMode::Feedback;
    else if (arg == "-i")
      mode = RunMode::FeedbackIdle;
    else if (arg == "-p")
      mode = RunMode::LowLatPresent;
    else if (arg == "-d" && i + 1 < argc)
      commit_delay_ms = std::atoi(argv[++i]);
    else {
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  App app{mode, commit_delay_ms};
  return app.Run();
}
