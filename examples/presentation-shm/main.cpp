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

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>  // wl_interface tables + wl::XdgWmBaseHandler / XdgSurfaceHandler<App> / XdgToplevelHandler<App>

// ── System Wayland C headers
// ──────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
}

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
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

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() — core Wayland interfaces
//
// wl_seat_traits::wl_iface() and wl_keyboard_traits::wl_iface() are provided
// inline by <wl/seat.hpp> (already included above).
// All xdg_shell traits wl_iface() implementations are provided inline by
// <wl/xdg_shell.hpp> (already included above).
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
// wp_presentation / wp_presentation_feedback wl_interface definitions
// ══════════════════════════════════════════════════════════════════════════════

extern const wl_interface wp_presentation_iface_def;
extern const wl_interface wp_presentation_feedback_iface_def;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* presentation_time_types[] = {
    nullptr,                              // [0] scalar
    &wl_surface_interface,                // [1] feedback → surface arg
    &wp_presentation_feedback_iface_def,  // [2] feedback → callback new_id
    &wl_output_interface,                 // [3] sync_output → output
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
static constexpr wl_message wp_presentation_requests[] = {
    {"destroy", "", nullptr},
    {"feedback", "on", &presentation_time_types[1]},
};
static constexpr wl_message wp_presentation_events[] = {
    {"clock_id", "u", &presentation_time_types[0]},
};
static constexpr wl_message wp_presentation_feedback_events[] = {
    {"sync_output", "o", &presentation_time_types[3]},
    {"presented", "uuuuuuu", &presentation_time_types[0]},
    {"discarded", "", nullptr},
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

/// Combine the protocol's split tv_sec_hi / tv_sec_lo into a timespec.
static void timespec_from_proto(timespec& ts,
                                uint32_t sec_hi,
                                uint32_t sec_lo,
                                uint32_t nsec) noexcept {
  ts.tv_sec =
      (static_cast<int64_t>(sec_hi) << 32) | static_cast<int64_t>(sec_lo);
  ts.tv_nsec = static_cast<long>(nsec);
}

static uint32_t timespec_to_ms(const timespec& ts) noexcept {
  return static_cast<uint32_t>(ts.tv_sec) * 1000u +
         static_cast<uint32_t>(ts.tv_nsec / 1'000'000L);
}

static int64_t timespec_diff_us(const timespec& a, const timespec& b) noexcept {
  return (a.tv_sec - b.tv_sec) * 1'000'000LL + (a.tv_nsec - b.tv_nsec) / 1000LL;
}

// ══════════════════════════════════════════════════════════════════════════════
// Pixel painting — identical to the Weston original
// ══════════════════════════════════════════════════════════════════════════════

/// Paint an animated spinning color wheel into the @p image (XRGB8888).
/// @p Phase drives the rotation; call with increasing values for animation.
static void paint_pixels(void* image,
                         int width,
                         int height,
                         const uint32_t phase) noexcept {
  const int halfh = height / 2;
  const int halfw = width / 2;
  auto* base = static_cast<uint32_t*>(image);

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
      const int idx = y * width + x;

      if (ox * ox + y2 > outer_r) {
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

// ── WlCompositorHandler
// ───────────────────────────────────────────────────────

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
  bool ProcessEvent(uint32_t, void**) override { return false; }
};

// ── WlShmPoolHandler
// ──────────────────────────────────────────────────────────

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

// ── XDG shell handlers provided by <wl/xdg_shell.hpp> ────────────────────────
//   wl::XdgWmBaseHandler        — responds to ping automatically
//   wl::XdgSurfaceHandler<App>  — acks configure, calls App::OnXdgSurfaceConfigure
//   wl::XdgToplevelHandler<App> — delegates configure/close to App

// ── WpPresentationHandler
// ─────────────────────────────────────────────────────

class WpPresentationHandler
    : public presentation_time::client::CWpPresentation<WpPresentationHandler> {
 public:
  clockid_t clk_id = CLOCK_MONOTONIC;
  void OnClockId(uint32_t id) override { clk_id = static_cast<clockid_t>(id); }
};

// ── WpPresentationFeedbackHandler
// ─────────────────────────────────────────────
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
  void OnPresented(uint32_t tv_sec_hi,
                   uint32_t tv_sec_lo,
                   uint32_t tv_nsec,
                   uint32_t refresh_ns,
                   uint32_t seq_hi,
                   uint32_t seq_lo,
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
  std::array<wl::WlPtr<WlBufferHandler>, static_cast<std::size_t>(kNumBuffers)>
      bufs;
  int next = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] bool Create(int w, int h, wl_proxy* shm_raw) noexcept;

  // Returns the mapped pixel data for buffer index i.
  [[nodiscard]] void* PixelData(int i) const noexcept {
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return static_cast<uint8_t*>(mem.data) +
           static_cast<std::size_t>(i) * stride *
               static_cast<std::size_t>(height);
  }

  // Finds and returns the next non-busy buffer index, or -1 if all are busy.
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
    std::fprintf(stderr, "presentation-shm: SHM allocation failed\n");
    return false;
  }

  // Create the pool.
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
        wl_shm_create_pool(reinterpret_cast<wl_shm*>(shm_raw), mem.fd,
                           static_cast<int>(total));
    if (!raw_pool) {
      std::fprintf(stderr, "presentation-shm: wl_shm_create_pool failed\n");
      return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
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

// Forward-declare so the App can hold a tracking pointer to the live feedkick
// handler (heap-allocated, self-deleting on callback).  Defined after App.
class FeedkickHandler;

class App {
 public:
  App(const RunMode mode, const int commit_delay_ms)
      : mode_(mode), commit_delay_ms_(commit_delay_ms) {}
  ~App();

  int Run();

  // ── Callbacks from CRTP handlers ────────────────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t /*width*/, int32_t /*height*/) noexcept {}  // fixed size
  void OnToplevelClose();
  void OnKey(uint32_t key, uint32_t state);
  void OnFrameDone(uint32_t stamp_ms) noexcept;
  void OnFeedbackPresented(WpPresentationFeedbackHandler& fb,
                           uint32_t tv_sec_hi,
                           uint32_t tv_sec_lo,
                           uint32_t tv_nsec,
                           uint32_t refresh_ns,
                           uint32_t seq_hi,
                           uint32_t seq_lo,
                           uint32_t flags) noexcept;
  void OnFeedbackDiscarded(WpPresentationFeedbackHandler& fb) noexcept;

  /// Called by FeedkickHandler to update the display refresh period estimate.
  void UpdateRefresh(const uint32_t refresh_ns) noexcept {
    refresh_nsec_ = refresh_ns;
  }

 private:
  // ── Configuration ────────────────────────────────────────────────────────
  RunMode mode_;
  int commit_delay_ms_ = 0;

  static constexpr int kWidth = 250;
  static constexpr int kHeight = 250;
  static constexpr int kRoundtripTimeoutMs = 5000;

  // ── Wayland objects (destruction order = reverse declaration) ────────────
  wl::DisplayHandle display_;

  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<WpPresentationHandler> presentation_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;

  // ── Input: seat + keyboard (optional; ESC quits) ─────────────────────────
  wl::SeatManager<App> seat_;

  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // Frame-pacing callback (feedback + feedback-idle modes).
  wl::WlPtr<WlCallbackHandler> frame_cb_;

  BufferPool pool_;

  // ── Application state ────────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  bool have_presentation_ = false;
  unsigned frame_seq_ = 0;               // monotone frame counter
  uint32_t refresh_nsec_ = 16'666'667u;  // 60 Hz default until feedback

  // Pending presentation-feedback objects (ownership transferred to the list).
  std::list<WpPresentationFeedbackHandler*> feedback_list_;
  // Last-presented feedback record (for p2p timing).
  WpPresentationFeedbackHandler* last_presented_ = nullptr;
  // Live feedkick handler in LowLatPresent mode (at most one at a time;
  // self-deletes on callback, so track it for cleanup on exit).
  FeedkickHandler* feedkick_ = nullptr;
  friend class FeedkickHandler;  // needs access to feedkick_

  // ── Global IDs from registry scan ────────────────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t presentation_name_ = 0, presentation_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  // wl_seat is tracked directly by seat_ (SeatManager::Record).

  // ── Pipeline ─────────────────────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateWindow();
  bool PreRender();
  bool MainLoop();
  void StartFeedbackMode();
  void StartPresentMode();

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
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  app_->OnFrameDone(time_ms);
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

// Set to 0 by signal_handler() on SIGINT so the event loop exits cleanly on
// the first Ctrl+C. Declared here (before App::Run) so the loop can read it.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
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
  if (!PreRender())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "presentation-shm: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
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
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "presentation-shm: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_ || !shm_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "presentation-shm: required globals not found\n");
    return false;
  }
  return true;
}

// ── BindGlobals
// ───────────────────────────────────────────────────────────────

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
  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "presentation-shm: wl_shm bind failed\n");
    return false;
  }

  // xdg_wm_base.
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "presentation-shm: xdg_wm_base bind failed\n");
    return false;
  }

  // wp_presentation — optional.
  if (presentation_name_) {
    if (wl::BindHandler<wp_presentation_traits>(
            registry_, presentation_, presentation_name_, presentation_ver_)) {
      have_presentation_ = true;
    }
  }
  if (!have_presentation_) {
    std::fprintf(stderr,
                 "presentation-shm: wp_presentation not available — "
                 "timing feedback disabled\n");
  }

  // wl_seat — optional; provides keyboard (ESC to quit).
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "presentation-shm: wl_seat bind failed\n");
    return false;
  }

  // Roundtrip so wl_shm.format, wp_presentation.clock_id, and seat
  // capabilities arrive.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
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

// ── CreateWindow
// ──────────────────────────────────────────────────────────────

bool App::CreateWindow() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_surface.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "presentation-shm: wl_compositor.create_surface failed\n");
    return false;
  }

  // xdg_surface.
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr,
                 "presentation-shm: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // xdg_toplevel.
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "presentation-shm: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;

  // Format title like the original.
  std::array<char, 128> title{};
  std::snprintf(title.data(), title.size(),
                "presentation-shm: %.*s [delay %d ms]",
                static_cast<int>(run_mode_name(mode_).size()),
                run_mode_name(mode_).data(), commit_delay_ms_);
  xdg_toplevel_.Get()->SetTitle(title.data());
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.presentation-shm");
  xdg_toplevel_.Get()->SetMinSize(kWidth, kHeight);
  xdg_toplevel_.Get()->SetMaxSize(kWidth, kHeight);

  // Commit to trigger the configure sequence.
  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "presentation-shm: timed out waiting for configure\n");
    return false;
  }

  return true;
}

// ── PreRender
// ─────────────────────────────────────────────────────────────────

bool App::PreRender() {
  // Create the buffer pool.
  if (!pool_.Create(kWidth, kHeight, shm_.Get()->GetProxy()))
    return false;

  // Pre-paint all buffers at evenly spaced phases (like the original).
  constexpr int timefactor = 1'000'000 / kNumBuffers;
  for (int i = 0; i < kNumBuffers; ++i) {
    paint_pixels(pool_.PixelData(i), kWidth, kHeight,
                 static_cast<uint32_t>(i * timefactor));
  }

  return true;
}

// ── Commit helpers
// ────────────────────────────────────────────────────────────

void App::EmulateRendering() const noexcept {
  if (commit_delay_ms_ <= 0)
    return;
  const timespec delay{.tv_sec = commit_delay_ms_ / 1000,
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

  // wp_presentation.feedback has protocol signature "on" — the surface object
  // argument comes FIRST, then the new_id. wl::construct<> always prepends
  // nullptr (the new_id placeholder) before user args, which is correct for
  // "no" requests but wrong for "on".  Use _MarshalNew directly so the args
  // are in wire order: (surface, nullptr).
  if (wl_proxy* raw = presentation_.Get()->_MarshalNew(
          wp_presentation_traits::Op::Feedback,
          &wp_presentation_feedback_traits::wl_iface(),
          surface_.Get()->GetProxy(), nullptr)) {
    fb->_SetProxy(raw);
    feedback_list_.push_back(fb);
  } else {
    delete fb;
  }
}

void App::CommitNext(uint32_t stamp_ms) noexcept {
  const int idx = pool_.NextFree();
  if (idx < 0) {
    std::fprintf(stderr,
                 "presentation-shm: all buffers busy — skipping frame\n");
    return;
  }

  surface_.Get()->Attach(
      pool_.bufs.at(static_cast<std::size_t>(idx)).Get()->GetProxy(), 0, 0);
  surface_.Get()->Damage(0, 0, kWidth, kHeight);
  surface_.Get()->Commit();
  pool_.bufs.at(static_cast<std::size_t>(idx)).Get()->busy = true;
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

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnKey(const uint32_t key, const uint32_t state) {
  if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED)
    running_ = false;
}

void App::OnFrameDone(const uint32_t stamp_ms) noexcept {
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
                              const uint32_t tv_sec_hi,
                              const uint32_t tv_sec_lo,
                              const uint32_t tv_nsec,
                              const uint32_t refresh_ns,
                              const uint32_t seq_hi,
                              const uint32_t seq_lo,
                              const uint32_t flags) noexcept {
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
  // We store 'present' in the commit field for p2p tracking below.
  const int64_t p2p = timespec_diff_us(present, *prev_present);
  const int64_t t2p = timespec_diff_us(present, fb.target);

  // Build flag string: s=vsync c=hw_clock e=hw_completion z=zero_copy.
  std::array<char, 8> flagstr{"____"};
  if (flags & 0x1u)
    flagstr.at(0) = 's';
  if (flags & 0x2u)
    flagstr.at(1) = 'c';
  if (flags & 0x4u)
    flagstr.at(2) = 'e';
  if (flags & 0x8u)
    flagstr.at(3) = 'z';

  switch (mode_) {
    case RunMode::LowLatPresent:
      std::printf("%6u: c2p %4u ms, p2p %5" PRId64 " us, t2p %6" PRId64
                  " us, [%s] seq %" PRIu64 "\n",
                  fb.frame_no, c2p, p2p, t2p, flagstr.data(), seq);
      break;
    case RunMode::Feedback:
    case RunMode::FeedbackIdle:
      std::printf("%6u: f2c %2u ms, c2p %2u ms, f2p %2u ms, p2p %5" PRId64
                  " us, t2p %6" PRId64 " us, [%s] seq %" PRIu64 "\n",
                  fb.frame_no, f2c, c2p, f2p, p2p, t2p, flagstr.data(), seq);
      break;
  }
  std::fflush(stdout);

  // Store the 'present' timestamp in the commit field so p2p works next round.
  fb.commit = present;

  // Remove from a list, transfer to last_presented_.
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

// ── Feedkick (low-latency mode)
// ────────────────────────────────────────────────
//
// In RUN_MODE_PRESENT, instead of using wl_surface.frame we wait for the
// wp_presentation_feedback.presented event of a "kick" feedback object, then
// immediately commit the next frame.  This gives us the minimum possible
// latency while still pacing to the display.

class FeedkickHandler
    : public presentation_time::client::CWpPresentationFeedback<
          FeedkickHandler> {
 public:
  App* app_ = nullptr;
  void OnSyncOutput(wl_proxy* /*out*/) override {}
  void OnPresented(uint32_t,
                   uint32_t,
                   uint32_t,
                   const uint32_t refresh_ns,
                   uint32_t,
                   uint32_t,
                   uint32_t) override {
    // Clear the App's tracking pointer before self-deleting so the destructor
    // doesn't double-free if App outlives this callback for any reason.
    app_->feedkick_ = nullptr;
    // Update refresh estimate and trigger the next low-latency commit.
    app_->UpdateRefresh(refresh_ns);
    wl_proxy_destroy(Detach());
    delete this;
  }
  void OnDiscarded() override {
    app_->feedkick_ = nullptr;
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
  // Same "on" signature fix as AttachPresentationFeedback: surface before
  // nullptr.
  if (wl_proxy* raw = presentation_.Get()->_MarshalNew(
          wp_presentation_traits::Op::Feedback,
          &wp_presentation_feedback_traits::wl_iface(),
          surface_.Get()->GetProxy(), nullptr)) {
    fk->_SetProxy(raw);
    feedkick_ = fk;  // take ownership; cleared by OnPresented/OnDiscarded
  } else {
    delete fk;
  }
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

// App::~App() is defined here (after FeedkickHandler is complete), so it can
// call feedkick_->Detach() and delete feedkick_ without using an incomplete
// type.
App::~App() {
  // Send versioned seat/keyboard release requests before member destructors
  // run.
  seat_.Release();
  // Destroy any live feedkick handler before the display disconnects.
  // FeedkickHandler is heap-allocated and normally self-deletes when its
  // wp_presentation_feedback callback fires; if the app exits before that
  // event (e.g., Ctrl+C), we must explicitly release the proxy and free the
  // object here so neither the proxy nor the C++ object is leaked.
  if (feedkick_) {
    if (wl_proxy* p = feedkick_->Detach())
      wl_proxy_destroy(p);
    delete feedkick_;
  }
  // Clean up pending feedback objects.
  for (const auto* fb : feedback_list_)
    delete fb;
  delete last_presented_;
}

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

bool App::MainLoop() {
  std::printf(
      "presentation-shm: mode=%.*s delay=%d ms "
      "(press ESC or Ctrl-C to quit)\n",
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

  // g_running is set to 0 by the SIGINT handler (first Ctrl+C).  Include it
  // in the stop predicate so the first signal exits cleanly.
  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; },
      "presentation-shm");

  std::fprintf(stderr, "presentation-shm exiting\n");
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

  auto mode = RunMode::Feedback;
  int commit_delay_ms = 0;

  // argv is a C-API parameter; pointer arithmetic on it is unavoidable.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (int i = 1; i < argc; ++i) {
    if (const std::string_view arg{argv[i]}; arg == "-f")
      mode = RunMode::Feedback;
    else if (arg == "-i")
      mode = RunMode::FeedbackIdle;
    else if (arg == "-p")
      mode = RunMode::LowLatPresent;
    else if (arg == "-d" && i + 1 < argc) {
      char* end = nullptr;
      commit_delay_ms = static_cast<int>(std::strtol(argv[++i], &end, 10));
    } else {
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  App app{mode, commit_delay_ms};
  return app.Run();
}
