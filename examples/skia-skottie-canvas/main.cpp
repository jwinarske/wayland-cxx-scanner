// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// skia-skottie-canvas — plays a Lottie animation with Skia's Skottie module on
// a bare wl_shm surface, with honest damage tracking and idle-commit
// suppression.
//
// Skia wraps the mapped pool memory with a raster surface (no copy) and Skottie
// renders the animation into it.  The damage submitted to the compositor is the
// animation's own dirty region, taken straight from Skottie's
// sksg::InvalidationController — not a heuristic.  When the animation holds (no
// keyframe changes) the controller reports an empty region, so the frame is
// neither rendered nor committed and the frame callback is not re-armed: the
// commit rate drops to zero until the animation moves again.  An
// animation-clock timerfd polls for the hold ending and for the next frame
// while playing.
//
// Controls:
//   ESC / window close        quit
//   SPACE / left-click / tap  pause / resume
//   Left / Right              scrub -/+ 1 s
//   F1                        toggle the performance overlay (also --hud)

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "fractional_scale_client.hpp"  // namespace fractional_scale_v1::client
#include "presentation_time_client.hpp"  // namespace presentation_time::client
#include "viewporter_client.hpp"         // namespace viewporter::client
#include "wayland_client.hpp"            // namespace wayland::client
#include "xdg_shell_client.hpp"          // namespace xdg_shell::client

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/cursor.hpp>
#include <wl/display.hpp>
#include <wl/presentation.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/scale_policy.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── Shared pacing + policy helpers
// ─────────────────────────────────────────────
#include "damage.hpp"
#include "frame_pacer.hpp"
#include "perf_hud.hpp"

// ── Skia + Skottie
// ─────────────────────────────────────────────────────────────
#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "modules/skottie/include/Skottie.h"
#include "modules/sksg/include/SkSGInvalidationController.h"

// ── System Wayland C headers
// ──────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
}

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() — core Wayland interfaces used by this example.
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
const wl_interface& wl_pointer_traits::wl_iface() noexcept {
  return wl_pointer_interface;
}
const wl_interface& wl_touch_traits::wl_iface() noexcept {
  return wl_touch_interface;
}

}  // namespace wayland::client

namespace {

// ══════════════════════════════════════════════════════════════════════════════
// SHM mapping — an anonymous memfd mmap'd for the pool.
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
    fd = memfd_create("skia-skottie-canvas", 0);
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
// Handlers
// ══════════════════════════════════════════════════════════════════════════════

class App;

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
};

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  std::uint32_t formats = 0;
  void OnFormat(std::uint32_t fmt) override {
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
  void OnDone(std::uint32_t time_ms) override;
};

// wp_viewporter / wp_viewport and the fractional-scale manager have no events;
// they are held only for their requests.
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
  void OnPreferredScale(std::uint32_t scale) override;
};

// ══════════════════════════════════════════════════════════════════════════════
// Buffer pool — a fixed number of wl_shm buffers from a single pool, recreated
// when the surface size changes.  Skia renders directly into the mapping.
// ══════════════════════════════════════════════════════════════════════════════

constexpr int kNumBuffers = 4;

struct BufferPool {
  ShmMapping mem;
  std::array<wl::WlPtr<WlBufferHandler>, static_cast<std::size_t>(kNumBuffers)>
      bufs;
  int next = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] bool Create(int w, int h, WlShmHandler& shm) noexcept;

  [[nodiscard]] std::size_t Stride() const noexcept {
    return static_cast<std::size_t>(width) * 4u;
  }

  [[nodiscard]] void* PixelData(int i) const noexcept {
    const std::size_t byte_offset = static_cast<std::size_t>(i) * Stride() *
                                    static_cast<std::size_t>(height);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return static_cast<std::uint8_t*>(mem.data) + byte_offset;
  }

  [[nodiscard]] int NextFree() noexcept {
    for (int attempt = 0; attempt < kNumBuffers; ++attempt) {
      if (const int idx = (next + attempt) % kNumBuffers;
          !bufs.at(static_cast<std::size_t>(idx)).Get()->busy) {
        next = (idx + 1) % kNumBuffers;
        return idx;
      }
    }
    return -1;
  }
};

bool BufferPool::Create(const int w, const int h, WlShmHandler& shm) noexcept {
  using namespace wayland::client;
  width = w;
  height = h;
  next = 0;

  // Release any buffers from a previous size before remapping.  The handler
  // objects persist, so clear their busy flags too — the new proxies start
  // free.
  for (auto& b : bufs) {
    b.Reset();
    b.Get()->busy = false;
  }

  const std::size_t stride = Stride();
  const std::size_t per_buf = stride * static_cast<std::size_t>(h);
  const std::size_t total = per_buf * static_cast<std::size_t>(kNumBuffers);

  if (!mem.Create(total)) {
    std::fprintf(stderr, "skia-skottie-canvas: SHM allocation failed\n");
    return false;
  }

  wl::WlPtr<WlShmPoolHandler> pool;
  if (wl_proxy* raw_pool =
          wl::construct<wl_shm_pool_traits, wl_shm_traits::Op::CreatePool>(
              shm, mem.fd, static_cast<int32_t>(total))) {
    pool.Attach(raw_pool);
  } else {
    std::fprintf(stderr, "skia-skottie-canvas: wl_shm.create_pool failed\n");
    return false;
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
      std::fprintf(
          stderr,
          "skia-skottie-canvas: wl_shm_pool.create_buffer [%d] failed\n", i);
      return false;
    }
  }

  pool.Reset();
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  App(demo::PacerConfig pacer_cfg,
      bool hud,
      sk_sp<skottie::Animation> anim,
      bool loop) noexcept
      : pacer_(pacer_cfg),
        anim_(std::move(anim)),
        anim_ms_total_(anim_ ? anim_->duration() * 1000.0 : 0.0),
        loop_(loop),
        fixed_dt_(pacer_cfg.fixed_dt) {
    hud_.set_visible(hud);
    last_advance_ms_ = NowMs();
    // At most the animation's dirty rect plus the HUD rect per frame.
    damage_logical_.reserve(2);
    damage_buffer_.reserve(2);
  }
  ~App() noexcept {
    if (timer_fd_ >= 0)
      ::close(timer_fd_);
  }

  int Run();

  // Callbacks from CRTP handlers.
  void OnXdgSurfaceConfigure(std::uint32_t serial);
  void OnToplevelConfigure(int32_t width, int32_t height) noexcept;
  void OnToplevelClose() { running_ = false; }
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameDone(std::uint32_t stamp_ms) noexcept;
  void OnPreferredScale(int scale_120) noexcept;
  // wp_presentation feedback: the frame committed for `fb.frame` turned to
  // light.  wl::PresentationManager creates the feedback when this hook exists.
  void OnPresented(const wl::PresentFeedback& fb) noexcept;
  // Pointer/touch input (SeatManager binds the wl_pointer / wl_touch when these
  // hooks are present).  Click or tap pauses/resumes; the enter hook caches the
  // serial and points the cursor at the default shape via wl::CursorManager.
  void OnPointerEnter(const wl::PointerEvent& ev) noexcept;
  void OnPointerButton(const wl::PointerButtonEvent& ev) noexcept;
  void OnTouchDown(const wl::TouchPoint& p) noexcept;

 private:
  static constexpr int kDefaultWidth = 480;
  static constexpr int kDefaultHeight = 320;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateWindow();
  bool MainLoop();
  bool RunSelfPaced();
  void PrintBenchmark() const noexcept;
  void PrintPresentSummary() const noexcept;

  [[nodiscard]] static double NowMs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1.0e6;
  }

  // Physical buffer size for the current logical size and scale.
  [[nodiscard]] wl::ScalePolicy::BufferSize BufferPx() const noexcept {
    return wl::ScalePolicy::ToBuffer(width_, height_, scale_120_);
  }
  [[nodiscard]] bool CanScale() const noexcept {
    return viewport_.Get()->GetProxy() != nullptr;
  }
  // Integer cursor scale: round the fractional scale up so the cursor stays
  // crisp (a 1.5 surface uses a 2× cursor).
  [[nodiscard]] int CursorScale() const noexcept {
    return (scale_120_ + wl::ScalePolicy::kUnityScale120 - 1) /
           wl::ScalePolicy::kUnityScale120;
  }

  bool EnsurePool() noexcept;
  void AdvanceClock() noexcept;
  // Seek the animation to the current playhead; returns its dirty region mapped
  // to logical pixels (empty when nothing changed — a hold).
  [[nodiscard]] SkIRect SeekDirty() noexcept;
  // Advance + seek + commit iff something is dirty.  Returns true if it
  // committed a frame.  The heart of idle-commit suppression.
  bool Produce(bool arm_callback) noexcept;
  void OnTimerTick() noexcept;
  // Wake the render loop after input (pause/scrub/hud) so a held or paused
  // frame repaints immediately rather than waiting for the timer.
  void Wake() noexcept;
  void RenderFrame(int idx) noexcept;
  void CommitFrame(bool arm_callback, const SkIRect& dirty_logical) noexcept;
  void SubmitDamage() noexcept;
  void AddHudDamage() noexcept;
  void ToggleHud() noexcept;
  void RequestFrameCallback() noexcept;
  void ApplyViewport() noexcept;
  // Logical destination box the animation is fit into (contain, centered).
  [[nodiscard]] SkRect AnimDstLogical() const noexcept;

  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<WpViewporterHandler> viewporter_;
  wl::WlPtr<WpFractionalScaleManagerHandler> fractional_manager_;
  wl::SeatManager<App> seat_;
  wl::CursorManager cursor_;  // set_cursor for the seat's pointer (HiDPI-aware)
  wl::PresentationManager<App> presentation_;

  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;
  wl::WlPtr<WpViewportHandler> viewport_;
  wl::WlPtr<WpFractionalScaleHandler> fractional_scale_;
  wl::WlPtr<WlCallbackHandler> frame_cb_;

  BufferPool pool_;

  std::uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  std::uint32_t shm_name_ = 0, shm_ver_ = 0;
  std::uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  std::uint32_t viewporter_name_ = 0, viewporter_ver_ = 0;
  std::uint32_t fractional_name_ = 0, fractional_ver_ = 0;

  bool running_ = true;
  bool configured_ = false;
  bool frame_pending_ = false;
  // Set when the buffer size, scale, or viewport destination must be
  // reapplied; forces a full-surface damage for the next frame.
  bool geometry_dirty_ = true;

  int width_ = kDefaultWidth;
  int height_ = kDefaultHeight;
  int pending_width_ = kDefaultWidth;
  int pending_height_ = kDefaultHeight;
  int scale_120_ = wl::ScalePolicy::kUnityScale120;
  uint32_t enter_serial_ = 0;  // last wl_pointer.enter serial, for set_cursor

  demo::FramePacer pacer_;
  demo::PerfHud hud_;
  demo::FpsMeter fps_;
  // Content-only rate/coverage (ticked in CommitFrame only when the animation
  // actually moved, never on HUD-forced repaints), so the on-canvas commit/s
  // and damage% stay honest — they fall to zero on a hold even while the
  // visible HUD keeps repainting.  Mirrors the once-a-second terminal readout.
  demo::FpsMeter commit_meter_;
  demo::DamageMeter damage_meter_;
  // Frames remaining over which the HUD region must stay damaged after a
  // visibility change, so the overlay clears from every rotating buffer.
  int hud_damage_frames_ = 0;
  std::vector<SkIRect> damage_logical_;  // dirty rects, logical px
  std::vector<SkIRect> damage_buffer_;   // mapped to buffer px for submission

  // ── Skottie playback ──────────────────────────────────────────────────────
  sk_sp<skottie::Animation> anim_;
  double anim_ms_total_ = 0.0;    // animation duration, ms
  double anim_ms_ = 0.0;          // current playhead, ms
  double last_advance_ms_ = 0.0;  // real clock at the last AdvanceClock()
  bool paused_ = false;
  bool loop_ = true;
  bool fixed_dt_ = false;
  int timer_fd_ = -1;
  std::uint64_t commit_count_ = 0;
  std::uint64_t last_report_commits_ = 0;
  double last_report_ms_ = 0.0;
};

void WpFractionalScaleHandler::OnPreferredScale(std::uint32_t scale) {
  app_->OnPreferredScale(static_cast<int>(scale));
}

void WlCallbackHandler::OnDone(std::uint32_t time_ms) {
  app_->OnFrameDone(time_ms);
}

// ── Signal handling
// ───────────────────────────────────────────────────────────

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_running = 1;

extern "C" void OnSigint(int /*signo*/) noexcept {
  g_running = 0;
}

// ── Pipeline
// ──────────────────────────────────────────────────────────────────

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  if (!CreateWindow())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "skia-skottie-canvas: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "skia-skottie-canvas: registry creation failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry&, std::uint32_t name,
                            std::string_view iface, std::uint32_t ver) {
    using namespace wayland::client;
    using namespace xdg_shell::client;

    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface ==
               viewporter::client::wp_viewporter_traits::interface_name) {
      viewporter_name_ = name;
      viewporter_ver_ = ver;
    } else if (iface ==
               fractional_scale_v1::client::
                   wp_fractional_scale_manager_v1_traits::interface_name) {
      fractional_name_ = name;
      fractional_ver_ = ver;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_.Record(name, ver);
    } else if (iface == presentation_time::client::wp_presentation_traits::
                            interface_name) {
      presentation_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "skia-skottie-canvas: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_ || !shm_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "skia-skottie-canvas: required globals not found\n");
    return false;
  }
  return true;
}

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "skia-skottie-canvas: wl_compositor bind failed\n");
    return false;
  }

  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "skia-skottie-canvas: wl_shm bind failed\n");
    return false;
  }

  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "skia-skottie-canvas: xdg_wm_base bind failed\n");
    return false;
  }

  // wp_viewporter and the fractional-scale manager are optional and have no
  // events, so bind and attach them like wl_compositor.  Without both, the
  // client stays at integer scale 1.
  if (viewporter_name_) {
    using T = viewporter::client::wp_viewporter_traits;
    if (wl_proxy* raw = registry_.Bind<T>(
            viewporter_name_, std::min(viewporter_ver_, T::version)))
      viewporter_.Attach(raw);
  }
  if (fractional_name_) {
    using T =
        fractional_scale_v1::client::wp_fractional_scale_manager_v1_traits;
    if (wl_proxy* raw = registry_.Bind<T>(
            fractional_name_, std::min(fractional_ver_, T::version)))
      fractional_manager_.Attach(raw);
  }

  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "skia-skottie-canvas: wl_seat bind failed\n");
    return false;
  }

  // wp_presentation is optional; Bind() is a no-op if it was never advertised.
  if (!presentation_.Bind(registry_, this)) {
    std::fprintf(stderr, "skia-skottie-canvas: wp_presentation bind failed\n");
    return false;
  }

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "skia-skottie-canvas: timed out waiting for formats\n");
    return false;
  }

  constexpr std::uint32_t kXrgb8888 = 1u;
  if (!(shm_.Get()->formats & (1u << kXrgb8888))) {
    std::fprintf(stderr,
                 "skia-skottie-canvas: WL_SHM_FORMAT_XRGB8888 not supported\n");
    return false;
  }

  // Cursor theme (optional): points the pointer at the default shape and
  // tracks the fractional scale for a crisp HiDPI cursor.
  if (!cursor_.Init(shm_.Get()->GetProxy(), compositor_.Get()->GetProxy(),
                    CursorScale())) {
    std::fprintf(stderr, "skia-skottie-canvas: cursor theme unavailable\n");
  }
  return true;
}

bool App::CreateWindow() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr,
                 "skia-skottie-canvas: wl_compositor.create_surface failed\n");
    return false;
  }

  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr,
                 "skia-skottie-canvas: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr,
                 "skia-skottie-canvas: xdg_surface.get_toplevel failed\n");
    return false;
  }
  auto* toplevel = xdg_toplevel_.Get();
  toplevel->app_ = this;
  toplevel->SetTitle("skia-skottie-canvas");
  toplevel->SetAppId("org.wayland-cxx.skia-skottie-canvas");

  // A viewport lets the surface present a physical-pixel buffer at a logical
  // destination size; the fractional-scale object reports the preferred scale.
  // Both are needed for fractional scaling — create them together or not.
  if (viewporter_.Get()->GetProxy() != nullptr &&
      fractional_manager_.Get()->GetProxy() != nullptr) {
    if (wl_proxy* raw = wl::construct<
            viewporter::client::wp_viewport_traits,
            viewporter::client::wp_viewporter_traits::Op::GetViewport>(
            *viewporter_.Get(), surface_.Get()->GetProxy())) {
      viewport_.Attach(raw);
    }
    if (!wl::SetupHandler(
            fractional_scale_,
            wl::construct<
                fractional_scale_v1::client::wp_fractional_scale_v1_traits,
                fractional_scale_v1::client::
                    wp_fractional_scale_manager_v1_traits::Op::
                        GetFractionalScale>(*fractional_manager_.Get(),
                                            surface_.Get()->GetProxy()))) {
      std::fprintf(stderr,
                   "skia-skottie-canvas: get_fractional_scale failed\n");
      return false;
    }
    fractional_scale_.Get()->app_ = this;
  }

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "skia-skottie-canvas: timed out waiting for configure\n");
    return false;
  }
  return true;
}

// ── Rendering
// ─────────────────────────────────────────────────────────────────

bool App::EnsurePool() noexcept {
  const wl::ScalePolicy::BufferSize px = BufferPx();
  if (pool_.width == px.width && pool_.height == px.height &&
      pool_.mem.data != MAP_FAILED)
    return true;
  return pool_.Create(px.width, px.height, *shm_.Get());
}

// The logical box the animation is fit into: contain (preserve aspect),
// centered within the current logical window.
SkRect App::AnimDstLogical() const noexcept {
  const float aw = anim_ ? anim_->size().width() : 1.0f;
  const float ah = anim_ ? anim_->size().height() : 1.0f;
  const float wl = static_cast<float>(width_);
  const float hl = static_cast<float>(height_);
  const float s = std::min(wl / aw, hl / ah);
  const float dw = aw * s;
  const float dh = ah * s;
  const float ox = (wl - dw) * 0.5f;
  const float oy = (hl - dh) * 0.5f;
  return SkRect::MakeXYWH(ox, oy, dw, dh);
}

void App::AdvanceClock() noexcept {
  const double now = NowMs();
  if (paused_) {
    last_advance_ms_ = now;
    return;
  }
  // Clamp real-time dt so a startup pause or a scheduling stall cannot fling
  // the playhead across the animation (and never advance backwards).
  const double dt = fixed_dt_ ? (1000.0 / 60.0)
                              : std::clamp(now - last_advance_ms_, 0.0, 100.0);
  last_advance_ms_ = now;
  if (anim_ms_total_ <= 0.0)
    return;
  anim_ms_ += dt;
  if (anim_ms_ >= anim_ms_total_) {
    if (loop_)
      anim_ms_ = std::fmod(anim_ms_, anim_ms_total_);
    else {
      anim_ms_ = anim_ms_total_;
      paused_ = true;  // hold on the last frame
    }
  }
}

SkIRect App::SeekDirty() noexcept {
  if (anim_ == nullptr)
    return SkIRect::MakeEmpty();
  sksg::InvalidationController ic;
  anim_->seekFrameTime(anim_ms_ / 1000.0, &ic);
  const SkRect b = ic.bounds();  // animation-local coordinates
  if (b.isEmpty())
    return SkIRect::MakeEmpty();
  // Map the animation-space dirty rect through the fit transform to logical px.
  const SkRect dst = AnimDstLogical();
  const float aw = anim_->size().width();
  const float ah = anim_->size().height();
  const float sx = dst.width() / aw;
  const float sy = dst.height() / ah;
  return SkRect::MakeLTRB(dst.left() + b.left() * sx, dst.top() + b.top() * sy,
                          dst.left() + b.right() * sx,
                          dst.top() + b.bottom() * sy)
      .roundOut();
}

bool App::Produce(bool arm_callback) noexcept {
  AdvanceClock();
  const SkIRect dirty = SeekDirty();
  const bool hud_dirty = hud_.visible() || hud_damage_frames_ > 0;
  if (!geometry_dirty_ && dirty.isEmpty() && !hud_dirty)
    return false;  // hold / paused: no render, no commit, no re-arm
  CommitFrame(arm_callback, dirty);
  return true;
}

void App::RenderFrame(int idx) noexcept {
  // wl_shm XRGB8888 is little-endian 0xXXRRGGBB: bytes B,G,R,X in memory, which
  // matches Skia's BGRA_8888 with an opaque alpha channel.
  const SkImageInfo info = SkImageInfo::Make(
      pool_.width, pool_.height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);

  sk_sp<SkSurface> surface =
      SkSurfaces::WrapPixels(info, pool_.PixelData(idx), pool_.Stride());
  if (surface == nullptr) {
    std::fprintf(stderr, "skia-skottie-canvas: failed to wrap pool memory\n");
    return;
  }

  // The buffer is physical pixels; the animation is placed in logical units, so
  // scale the canvas once at the top.  Damage comes back in logical pixels.
  const auto canvas_scale =
      static_cast<SkScalar>(wl::ScalePolicy::CanvasScale(scale_120_));
  SkCanvas* canvas = surface->getCanvas();
  canvas->scale(canvas_scale, canvas_scale);

  // Full repaint each committed frame: clear the backdrop and draw the
  // animation (already seeked by SeekDirty) fit-centered into the logical box.
  canvas->clear(SkColorSetRGB(0x14, 0x18, 0x22));
  if (anim_ != nullptr) {
    const SkRect dst = AnimDstLogical();
    anim_->render(canvas, &dst);
  }
  // The HUD draws in the same logical space; it is a no-op while hidden.
  hud_.Render(canvas, pacer_, fps_.fps());
}

void App::CommitFrame(bool arm_callback,
                      const SkIRect& dirty_logical) noexcept {
  if (!EnsurePool())
    return;

  const int idx = pool_.NextFree();
  if (idx < 0)
    return;  // every buffer held by the compositor; drop the frame

  const double now = NowMs();
  fps_.Tick(now);

  // A frame carries real content when the geometry changed or the animation
  // reported a non-empty dirty region; a HUD-forced repaint (empty dirty, no
  // geometry change) must not count, or the on-canvas commit/s and damage%
  // would never fall to zero on a hold.  The damage fraction is the animation's
  // own dirty area over the logical surface (a full geometry repaint is 100%).
  const bool content = geometry_dirty_ || !dirty_logical.isEmpty();
  if (content) {
    commit_meter_.Tick(now);
    const double area =
        static_cast<double>(width_) * static_cast<double>(height_);
    const double frac = geometry_dirty_ || area <= 0.0
                            ? 1.0
                            : static_cast<double>(dirty_logical.width()) *
                                  static_cast<double>(dirty_logical.height()) /
                                  area;
    damage_meter_.Tick(now, frac);
  }

  // Refresh the HUD's extra lines before it is drawn.  Kept populated so the
  // panel (and Bounds()) stays a stable two rows taller than the standard
  // stats, which lets AddHudDamage clear the whole overlay when it is hidden.
  std::array<char, 32> commit_line{};
  std::array<char, 32> damage_line{};
  std::snprintf(commit_line.data(), commit_line.size(), "commit %3.0f/s",
                commit_meter_.fps());
  std::snprintf(damage_line.data(), damage_line.size(), "damage %5.1f%%",
                damage_meter_.mean_fraction() * 100.0);
  hud_.SetExtraLine(0, commit_line.data());
  hud_.SetExtraLine(1, damage_line.data());

  RenderFrame(idx);

  auto& buf = *pool_.bufs.at(static_cast<std::size_t>(idx)).Get();
  auto* surface = surface_.Get();
  surface->Attach(buf.GetProxy(), 0, 0);
  if (geometry_dirty_) {
    ApplyViewport();
    surface->DamageBuffer(0, 0, pool_.width, pool_.height);
    geometry_dirty_ = false;
  } else {
    damage_logical_.clear();
    if (!dirty_logical.isEmpty())
      damage_logical_.push_back(dirty_logical);
    AddHudDamage();
    SubmitDamage();
  }
  if (arm_callback)
    RequestFrameCallback();
  // Request presentation feedback for this content before committing it.
  presentation_.Arm(surface->GetProxy(), pacer_.frame());
  surface->Commit();
  buf.busy = true;
  frame_pending_ = arm_callback;
  ++commit_count_;
  pacer_.Advance();
}

// Maps the scene's logical dirty rects to buffer pixels and emits one
// damage_buffer per rect, clamped to the buffer and coalesced.
void App::SubmitDamage() noexcept {
  const auto scale =
      static_cast<float>(wl::ScalePolicy::CanvasScale(scale_120_));

  damage_buffer_.clear();
  damage_buffer_.reserve(damage_logical_.size());
  for (const SkIRect& r : damage_logical_) {
    const SkRect scaled = SkRect::MakeLTRB(
        static_cast<float>(r.fLeft) * scale, static_cast<float>(r.fTop) * scale,
        static_cast<float>(r.fRight) * scale,
        static_cast<float>(r.fBottom) * scale);
    damage_buffer_.push_back(scaled.roundOut());
  }

  demo::ClampToBounds(damage_buffer_,
                      SkIRect::MakeWH(pool_.width, pool_.height));
  demo::Coalesce(damage_buffer_);

  for (const SkIRect& r : damage_buffer_)
    surface_.Get()->DamageBuffer(r.fLeft, r.fTop, r.width(), r.height());
}

// Adds the HUD's fixed region to the per-frame damage list.  While visible the
// FPS counter changes every frame, so the region is damaged continuously; after
// the HUD is hidden it is damaged for a few more frames so the overlay clears
// from every rotating buffer (each buffer must be repainted once).
void App::AddHudDamage() noexcept {
  if (hud_.visible()) {
    damage_logical_.push_back(hud_.Bounds());
  } else if (hud_damage_frames_ > 0) {
    damage_logical_.push_back(hud_.Bounds());
    --hud_damage_frames_;
  }
}

void App::ToggleHud() noexcept {
  hud_.toggle();
  // Repaint the HUD region across all buffers so a hidden HUD leaves no residue
  // and a shown one appears at once.
  hud_damage_frames_ = kNumBuffers;
}

void App::ApplyViewport() noexcept {
  if (viewport_.Get()->GetProxy() != nullptr)
    viewport_.Get()->SetDestination(width_, height_);
}

void App::RequestFrameCallback() noexcept {
  using wl_s = wayland::client::wl_surface_traits;
  using wl_c = wayland::client::wl_callback_traits;
  if (wl_proxy* raw = wl::construct<wl_c, wl_s::Op::Frame>(*surface_.Get())) {
    frame_cb_.Get()->app_ = this;
    frame_cb_.Get()->_SetProxy(raw);
  }
}

// ── Event callbacks
// ───────────────────────────────────────────────────────────

void App::OnToplevelConfigure(int32_t width, int32_t height) noexcept {
  // A zero dimension means "pick your own size"; keep the current one.
  pending_width_ = width > 0 ? width : width_;
  pending_height_ = height > 0 ? height : height_;
}

void App::OnXdgSurfaceConfigure(std::uint32_t /*serial*/) {
  // XdgSurfaceHandler acks the configure for us; apply the negotiated size.
  if (pending_width_ != width_ || pending_height_ != height_)
    geometry_dirty_ = true;
  width_ = pending_width_;
  height_ = pending_height_;
  configured_ = true;
  // In a self-paced run RunSelfPaced() drives every frame; configure only
  // records the size.
  if (!pacer_.self_paced() && !frame_pending_)
    Produce(/*arm_callback=*/false);
}

void App::OnPreferredScale(int scale_120) noexcept {
  // Only honor fractional scale when a viewport is available to present the
  // physical buffer at the logical size.
  if (!CanScale() || scale_120 <= 0 || scale_120 == scale_120_)
    return;
  scale_120_ = scale_120;
  cursor_.SetScale(CursorScale());  // keep the cursor crisp at the new scale
  geometry_dirty_ = true;
  if (!pacer_.self_paced() && !frame_pending_ && configured_)
    Produce(/*arm_callback=*/false);
}

// Repaint promptly after input even when playback is paused or holding — the
// change (pause overlay, scrub, HUD) must not wait for the next timer tick.
void App::Wake() noexcept {
  if (pacer_.self_paced() || frame_pending_ || !configured_)
    return;
  Produce(/*arm_callback=*/false);
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;
  switch (ev.key) {
    case KEY_ESC:
      running_ = false;
      return;
    case KEY_SPACE:
      paused_ = !paused_;
      geometry_dirty_ = true;  // force one repaint so the state is visible
      Wake();
      return;
    case KEY_LEFT:
      anim_ms_ = std::max(0.0, anim_ms_ - 1000.0);
      geometry_dirty_ = true;
      Wake();
      return;
    case KEY_RIGHT:
      if (anim_ms_total_ > 0.0)
        anim_ms_ = std::min(anim_ms_total_, anim_ms_ + 1000.0);
      geometry_dirty_ = true;
      Wake();
      return;
    case KEY_F1:
      ToggleHud();
      Wake();
      return;
    default:
      return;
  }
}

void App::OnPointerEnter(const wl::PointerEvent& ev) noexcept {
  enter_serial_ = ev.serial;  // set_cursor must carry an enter serial
  cursor_.Reset();            // the compositor resets the cursor on enter
  cursor_.Set(seat_.Pointer(), enter_serial_, "default");
}

void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED || ev.button != BTN_LEFT)
    return;
  paused_ = !paused_;  // click anywhere pauses/resumes
  geometry_dirty_ = true;
  Wake();
}

void App::OnTouchDown(const wl::TouchPoint& /*p*/) noexcept {
  paused_ = !paused_;  // tap anywhere pauses/resumes
  geometry_dirty_ = true;
  Wake();
}

void App::OnFrameDone(std::uint32_t /*stamp_ms*/) noexcept {
  // Destroy the spent callback proxy before arming the next frame.
  wl_proxy* const spent = frame_cb_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent)
      wl_proxy_destroy(spent);
  }};

  frame_pending_ = false;
  // Produce the next frame; when the animation is holding, Produce() commits
  // nothing and does not re-arm the callback — the timer takes over polling.
  if (running_ && configured_)
    Produce(/*arm_callback=*/false);
}

// The animation-clock timer: while a frame callback is in flight it does
// nothing (the callback paces production); once production has gone idle (a
// hold, with no callback outstanding) each tick re-checks whether the animation
// has started moving again and resumes committing when it has.
void App::OnTimerTick() noexcept {
  std::uint64_t expirations = 0;
  if (timer_fd_ >= 0) {
    const ssize_t n = ::read(timer_fd_, &expirations, sizeof(expirations));
    (void)n;
  }
  // Once per second, report the commit rate so idle-commit suppression is
  // visible from the terminal: it drops to 0 while the animation holds or is
  // paused, and returns to the refresh rate while it plays.
  const double now = NowMs();
  if (now - last_report_ms_ >= 1000.0) {
    const std::uint64_t committed = commit_count_ - last_report_commits_;
    std::printf("skia-skottie-canvas: %2llu commits/s  playhead=%.2fs%s\n",
                static_cast<unsigned long long>(committed), anim_ms_ / 1000.0,
                paused_ ? "  [paused]" : (committed == 0 ? "  [holding]" : ""));
    std::fflush(stdout);
    last_report_ms_ = now;
    last_report_commits_ = commit_count_;
  }

  if (!running_ || !configured_ || frame_pending_)
    return;
  Produce(/*arm_callback=*/false);
}

void App::OnPresented(const wl::PresentFeedback& fb) noexcept {
  // Real commit→turn-to-light latency and the compositor's measured refresh.
  pacer_.RecordPresentMs(fb.latency_ms);
  pacer_.NoteRefreshNs(fb.refresh_ns);
}

// Renders a bounded number of frames back-to-back, driving each with a display
// roundtrip instead of a compositor frame callback so the run completes even
// when the surface is never presented (headless, occluded).
bool App::RunSelfPaced() {
  std::printf("skia-skottie-canvas: self-paced run%s\n",
              pacer_.benchmarking() ? " (benchmark)" : "");
  constexpr int kMaxStalls = 8;
  int stalls = 0;
  while (running_ && g_running && !pacer_.reached_limit()) {
    const std::uint32_t before = pacer_.frame();
    const double t0 = NowMs();
    // Self-paced/benchmark renders every frame (idle suppression is an
    // interactive feature); force a commit regardless of the dirty region.
    AdvanceClock();
    const SkIRect dirty = SeekDirty();
    CommitFrame(/*arm_callback=*/false, dirty);
    // Time only render + commit; the roundtrip below is compositor latency, not
    // render cost, so it is excluded from the frame-time sample.
    const double render_ms = NowMs() - t0;

    // The roundtrip flushes the commit and lets buffer releases arrive.
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      std::fprintf(stderr, "skia-skottie-canvas: roundtrip failed\n");
      return false;
    }

    if (pacer_.frame() == before) {
      // Frame dropped (pool momentarily exhausted); the roundtrip should have
      // freed a buffer.  Bound the retries so a wedged compositor cannot spin
      // this loop forever.
      if (++stalls > kMaxStalls) {
        std::fprintf(stderr, "skia-skottie-canvas: buffer pool stalled\n");
        return false;
      }
      continue;
    }
    stalls = 0;
    pacer_.RecordFrameMs(render_ms);
  }
  if (pacer_.benchmarking())
    PrintBenchmark();
  return true;
}

void App::PrintBenchmark() const noexcept {
  std::printf(
      "skia-skottie-canvas: %zu frames  mean=%.3f ms  p50=%.3f  p95=%.3f  "
      "p99=%.3f\n",
      pacer_.sample_count(), pacer_.Mean(), pacer_.Percentile(50),
      pacer_.Percentile(95), pacer_.Percentile(99));
  PrintPresentSummary();
}

// Reports wp_presentation timing when any frame was actually presented.  In a
// headless or fully occluded run the compositor discards every frame, so this
// stays silent rather than printing zeros.
void App::PrintPresentSummary() const noexcept {
  if (pacer_.present_count() == 0)
    return;
  std::printf(
      "skia-skottie-canvas: presentation: %llu shown  latency mean=%.3f ms  "
      "p50=%.3f  p95=%.3f  refresh=%.2f Hz\n",
      static_cast<unsigned long long>(pacer_.present_count()),
      pacer_.PresentMean(), pacer_.PresentPercentile(50),
      pacer_.PresentPercentile(95), pacer_.refresh_hz());
}

bool App::MainLoop() {
  if (pacer_.self_paced())
    return RunSelfPaced();

  // Animation-clock timer: fires at ~60 Hz.  It paces production and,
  // crucially, keeps polling after production goes idle (a hold) so the loop
  // wakes when the animation moves again — without it, suppressing commits
  // would also suppress the frame callbacks that would otherwise resume the
  // loop.
  timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd_ < 0) {
    std::fprintf(stderr, "skia-skottie-canvas: timerfd_create failed\n");
    return false;
  }
  constexpr long kFrameNs = 1'000'000'000L / 60;
  const itimerspec spec{{0, kFrameNs}, {0, kFrameNs}};
  if (::timerfd_settime(timer_fd_, 0, &spec, nullptr) < 0) {
    std::fprintf(stderr, "skia-skottie-canvas: timerfd_settime failed\n");
    return false;
  }

  std::printf(
      "skia-skottie-canvas: playing (%.1fs @ %.0f fps).  SPACE/click = pause, "
      "arrows = scrub, ESC = quit\n",
      anim_ ? anim_->duration() : 0.0, anim_ ? anim_->fps() : 0.0);
  last_advance_ms_ = NowMs();
  // Kick off the first frame.
  Produce(/*arm_callback=*/false);

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; },
      "skia-skottie-canvas",
      {wl::FdSource{[this] { return seat_.GetRepeatFd(); },
                    [this] { seat_.DispatchRepeat(); }},
       wl::FdSource{[this] { return timer_fd_; }, [this] { OnTimerTick(); }},
       wl::FdSource{[this] { return cursor_.FrameFd(); },
                    [this] { cursor_.DispatchFrame(); }}});
  PrintPresentSummary();
  return ok;
}

}  // namespace

namespace {

void PrintUsage() {
  std::printf(
      "usage: skia_skottie_canvas --lottie FILE [--no-loop] [--hud]\n"
      "                           [--frames N] [--exit] [--fixed-dt] "
      "[--benchmark N]\n"
      "  --lottie FILE  Lottie/Bodymovin .json to play (required)\n"
      "  --no-loop      stop on the last frame instead of looping\n"
      "  --hud          show the performance overlay (toggle with F1)\n"
      "  --frames N     render at most N frames (self-paced)\n"
      "  --exit         quit once the frame limit is reached\n"
      "  --fixed-dt     deterministic 60 Hz animation clock\n"
      "  --benchmark N  render N frames self-paced and print frame-time "
      "stats\n"
      "\n"
      "No animation is bundled: supply your own Lottie .json.  Skia ships a\n"
      "sample corpus under resources/skottie/ and airbnb/lottie-web has "
      "more.\n");
}

[[nodiscard]] bool ParseArgs(const std::vector<std::string_view>& args,
                             demo::PacerConfig& cfg,
                             bool& hud,
                             std::string& lottie,
                             bool& loop) {
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    // Parses the next argument as a positive frame count.  Rejecting <= 0 (and
    // absurdly large values) keeps a bounded, self-paced run from looping
    // forever.
    constexpr long kMaxFrames = 1'000'000;
    const auto next_int = [&](int& out) {
      if (i + 1 >= args.size())
        return false;
      const std::string s(args[i + 1]);
      char* end = nullptr;
      const long value = std::strtol(s.c_str(), &end, 10);
      if (end == s.c_str() || value < 1 || value > kMaxFrames)
        return false;
      out = static_cast<int>(value);
      ++i;
      return true;
    };
    if (a == "--lottie") {
      if (i + 1 >= args.size())
        return false;
      lottie = std::string(args[++i]);
    } else if (a == "--no-loop") {
      loop = false;
    } else if (a == "--frames") {
      if (!next_int(cfg.max_frames))
        return false;
    } else if (a == "--exit") {
      cfg.exit_on_limit = true;
    } else if (a == "--fixed-dt") {
      cfg.fixed_dt = true;
    } else if (a == "--benchmark") {
      if (!next_int(cfg.max_frames))
        return false;
      cfg.benchmark = true;
      cfg.exit_on_limit = true;
    } else if (a == "--hud") {
      hud = true;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, OnSigint);

  const std::vector<std::string_view> args(argv, std::next(argv, argc));
  demo::PacerConfig cfg;
  bool hud = false;
  bool loop = true;
  std::string lottie;
  if (!ParseArgs(args, cfg, hud, lottie, loop)) {
    PrintUsage();
    return EXIT_FAILURE;
  }
  if (lottie.empty()) {
    std::fprintf(stderr, "skia-skottie-canvas: --lottie FILE is required\n");
    PrintUsage();
    return EXIT_FAILURE;
  }

  sk_sp<skottie::Animation> anim =
      skottie::Animation::Builder().makeFromFile(lottie.c_str());
  if (anim == nullptr) {
    std::fprintf(stderr, "skia-skottie-canvas: failed to load Lottie '%s'\n",
                 lottie.c_str());
    return EXIT_FAILURE;
  }

  App app(cfg, hud, std::move(anim), loop);
  return app.Run();
}
