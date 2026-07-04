// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// skia-shm-canvas — renders the shared demo scene with Skia's CPU raster
// backend straight into a wl_shm buffer.
//
// Skia wraps the mapped pool memory with a raster surface (no copy), draws the
// scene, and the frame is committed with buffer damage.  Frame-callback pacing
// keeps the render loop in step with the compositor.  This is the software
// path; the GL and Vulkan variants render the same scene for direct
// comparison.
//
// Controls:
//   ESC / window close   quit
//   SPACE / left-click  toggles the button-active scene state (click the
//   button)

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "fractional_scale_client.hpp"  // namespace fractional_scale_v1::client
#include "viewporter_client.hpp"        // namespace viewporter::client
#include "wayland_client.hpp"           // namespace wayland::client
#include "xdg_shell_client.hpp"         // namespace xdg_shell::client

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── Shared scene + policy helpers
// ─────────────────────────────────────────────
#include "damage.hpp"
#include "frame_pacer.hpp"
#include "scale.hpp"
#include "scene.hpp"
#include "view_tree.hpp"

// ── Skia
// ──────────────────────────────────────────────────────────────────────
#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"

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
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
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
    fd = memfd_create("skia-shm-canvas", 0);
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
    std::fprintf(stderr, "skia-shm-canvas: SHM allocation failed\n");
    return false;
  }

  wl::WlPtr<WlShmPoolHandler> pool;
  if (wl_proxy* raw_pool =
          wl::construct<wl_shm_pool_traits, wl_shm_traits::Op::CreatePool>(
              shm, mem.fd, static_cast<int32_t>(total))) {
    pool.Attach(raw_pool);
  } else {
    std::fprintf(stderr, "skia-shm-canvas: wl_shm.create_pool failed\n");
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
      std::fprintf(stderr,
                   "skia-shm-canvas: wl_shm_pool.create_buffer [%d] failed\n",
                   i);
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
  explicit App(demo::PacerConfig pacer_cfg) : pacer_(pacer_cfg) {
    // Views are the only damage sources, so the per-frame damage lists never
    // exceed the view count.  Reserve once so steady-state frames allocate
    // nothing.
    const auto max_rects = static_cast<std::size_t>(demo::View::kCount);
    damage_logical_.reserve(max_rects);
    damage_buffer_.reserve(max_rects);
  }

  int Run();

  // Callbacks from CRTP handlers.
  void OnXdgSurfaceConfigure(std::uint32_t serial);
  void OnToplevelConfigure(int32_t width, int32_t height) noexcept;
  void OnToplevelClose() { running_ = false; }
  void OnKey(const wl::KeyEvent& ev);
  void OnFrameDone(std::uint32_t stamp_ms) noexcept;
  void OnPreferredScale(int scale_120) noexcept;
  // Pointer input is delivered by wl::SeatManager (it creates the wl_pointer
  // when this hook is present); the event carries the click position.
  void OnPointerButton(const wl::PointerButtonEvent& ev) noexcept;
  // A touch tap on the button toggles it too (SeatManager creates the
  // wl_touch when this hook is present).
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

  [[nodiscard]] static double NowMs() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1.0e6;
  }

  // Physical buffer size for the current logical size and scale.
  [[nodiscard]] demo::BufferSize BufferPx() const noexcept {
    return demo::ScalePolicy::ToBuffer(width_, height_, scale_120_);
  }
  [[nodiscard]] bool CanScale() const noexcept {
    return viewport_.Get()->GetProxy() != nullptr;
  }

  bool EnsurePool() noexcept;
  void RenderFrame(int idx) noexcept;
  void CommitFrame(bool arm_callback) noexcept;
  void SubmitDamage() noexcept;
  void RequestFrameCallback() noexcept;
  void ApplyViewport() noexcept;

  wl::DisplayHandle display_;
  wl::CRegistry registry_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlShmHandler> shm_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<WpViewporterHandler> viewporter_;
  wl::WlPtr<WpFractionalScaleManagerHandler> fractional_manager_;
  wl::SeatManager<App> seat_;

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
  int scale_120_ = demo::ScalePolicy::kUnityScale120;

  demo::SceneState scene_;
  demo::FramePacer pacer_;
  demo::ViewTree view_tree_;
  std::vector<SkIRect> damage_logical_;  // dirty view rects, logical px
  std::vector<SkIRect> damage_buffer_;   // mapped to buffer px for submission
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
    std::fprintf(stderr, "skia-shm-canvas: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "skia-shm-canvas: registry creation failed\n");
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
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "skia-shm-canvas: timed out waiting for globals\n");
    return false;
  }

  if (!compositor_name_ || !shm_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr, "skia-shm-canvas: required globals not found\n");
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
    std::fprintf(stderr, "skia-shm-canvas: wl_compositor bind failed\n");
    return false;
  }

  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "skia-shm-canvas: wl_shm bind failed\n");
    return false;
  }

  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "skia-shm-canvas: xdg_wm_base bind failed\n");
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
    std::fprintf(stderr, "skia-shm-canvas: wl_seat bind failed\n");
    return false;
  }

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "skia-shm-canvas: timed out waiting for formats\n");
    return false;
  }

  constexpr std::uint32_t kXrgb8888 = 1u;
  if (!(shm_.Get()->formats & (1u << kXrgb8888))) {
    std::fprintf(stderr,
                 "skia-shm-canvas: WL_SHM_FORMAT_XRGB8888 not supported\n");
    return false;
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
                 "skia-shm-canvas: wl_compositor.create_surface failed\n");
    return false;
  }

  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr,
                 "skia-shm-canvas: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "skia-shm-canvas: xdg_surface.get_toplevel failed\n");
    return false;
  }
  auto* toplevel = xdg_toplevel_.Get();
  toplevel->app_ = this;
  toplevel->SetTitle("skia-shm-canvas");
  toplevel->SetAppId("org.wayland-cxx.skia-shm-canvas");

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
      std::fprintf(stderr, "skia-shm-canvas: get_fractional_scale failed\n");
      return false;
    }
    fractional_scale_.Get()->app_ = this;
  }

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr, "skia-shm-canvas: timed out waiting for configure\n");
    return false;
  }
  return true;
}

// ── Rendering
// ─────────────────────────────────────────────────────────────────

bool App::EnsurePool() noexcept {
  const demo::BufferSize px = BufferPx();
  if (pool_.width == px.width && pool_.height == px.height &&
      pool_.mem.data != MAP_FAILED)
    return true;
  return pool_.Create(px.width, px.height, *shm_.Get());
}

void App::RenderFrame(int idx) noexcept {
  // wl_shm XRGB8888 is little-endian 0xXXRRGGBB: bytes B,G,R,X in memory, which
  // matches Skia's BGRA_8888 with an opaque alpha channel.
  const SkImageInfo info = SkImageInfo::Make(
      pool_.width, pool_.height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);

  sk_sp<SkSurface> surface =
      SkSurfaces::WrapPixels(info, pool_.PixelData(idx), pool_.Stride());
  if (surface == nullptr) {
    std::fprintf(stderr, "skia-shm-canvas: failed to wrap pool memory\n");
    return;
  }

  // The buffer is physical pixels; the scene draws in logical units, so scale
  // the canvas once at the top.  Damage comes back in logical pixels.
  const auto canvas_scale =
      static_cast<SkScalar>(demo::ScalePolicy::CanvasScale(scale_120_));
  SkCanvas* canvas = surface->getCanvas();
  canvas->scale(canvas_scale, canvas_scale);

  demo::DemoScene::Render(canvas, scene_, view_tree_);
}

void App::CommitFrame(bool arm_callback) noexcept {
  if (!EnsurePool())
    return;

  const int idx = pool_.NextFree();
  if (idx < 0) {
    // Every buffer is held by the compositor.  Drop the frame rather than grow
    // the pool unboundedly; frame-callback pacing keeps at most one frame in
    // flight, so with kNumBuffers buffers this is not expected to be reached.
    return;
  }

  scene_.frame = pacer_.frame();
  // Lay out the views for the current logical size and mark the animated
  // spinner dirty; the button is marked dirty by input.
  view_tree_.Layout(width_, height_);
  view_tree_.MarkDirty(demo::View::kSpinner);
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
    view_tree_.CollectDamage(damage_logical_);
    SubmitDamage();
  }
  if (arm_callback)
    RequestFrameCallback();
  surface->Commit();
  buf.busy = true;
  frame_pending_ = arm_callback;
  view_tree_.ClearDirty();
  pacer_.Advance();
}

// Maps the scene's logical dirty rects to buffer pixels and emits one
// damage_buffer per rect, clamped to the buffer and coalesced.
void App::SubmitDamage() noexcept {
  const auto scale =
      static_cast<float>(demo::ScalePolicy::CanvasScale(scale_120_));

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
    CommitFrame(/*arm_callback=*/true);
}

void App::OnPreferredScale(int scale_120) noexcept {
  // Only honor fractional scale when a viewport is available to present the
  // physical buffer at the logical size.
  if (!CanScale() || scale_120 <= 0 || scale_120 == scale_120_)
    return;
  scale_120_ = scale_120;
  geometry_dirty_ = true;
  if (!pacer_.self_paced() && !frame_pending_ && configured_)
    CommitFrame(/*arm_callback=*/true);
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;
  if (ev.key == KEY_ESC) {
    running_ = false;
  } else if (ev.key == KEY_SPACE) {
    scene_.button_active = !scene_.button_active;
    view_tree_.MarkDirty(demo::View::kButton);
  }
}

void App::OnPointerButton(const wl::PointerButtonEvent& ev) noexcept {
  if (ev.state != WL_POINTER_BUTTON_STATE_PRESSED || ev.button != BTN_LEFT)
    return;
  // Pointer coordinates are surface-local logical pixels — the same space the
  // view tree is laid out in (the viewport maps the physical buffer to it).
  if (view_tree_.HitTest(static_cast<SkScalar>(ev.x),
                         static_cast<SkScalar>(ev.y)) == demo::View::kButton) {
    scene_.button_active = !scene_.button_active;
    view_tree_.MarkDirty(demo::View::kButton);
  }
}

void App::OnTouchDown(const wl::TouchPoint& p) noexcept {
  if (view_tree_.HitTest(static_cast<SkScalar>(p.x),
                         static_cast<SkScalar>(p.y)) == demo::View::kButton) {
    scene_.button_active = !scene_.button_active;
    view_tree_.MarkDirty(demo::View::kButton);
  }
}

void App::OnFrameDone(std::uint32_t /*stamp_ms*/) noexcept {
  // Destroy the spent callback proxy before arming the next frame.
  wl_proxy* const spent = frame_cb_.Detach();
  const auto guard = wl::ScopeExit{[spent] {
    if (spent)
      wl_proxy_destroy(spent);
  }};

  frame_pending_ = false;
  if (running_ && configured_)
    CommitFrame(/*arm_callback=*/true);
}

// Renders a bounded number of frames back-to-back, driving each with a display
// roundtrip instead of a compositor frame callback so the run completes even
// when the surface is never presented (headless, occluded).
bool App::RunSelfPaced() {
  std::printf("skia-shm-canvas: self-paced run%s\n",
              pacer_.benchmarking() ? " (benchmark)" : "");
  constexpr int kMaxStalls = 8;
  int stalls = 0;
  while (running_ && g_running && !pacer_.reached_limit()) {
    const std::uint32_t before = pacer_.frame();
    const double t0 = NowMs();
    CommitFrame(/*arm_callback=*/false);
    // Time only render + commit; the roundtrip below is compositor latency, not
    // render cost, so it is excluded from the frame-time sample.
    const double render_ms = NowMs() - t0;

    // The roundtrip flushes the commit and lets buffer releases arrive.
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      std::fprintf(stderr, "skia-shm-canvas: roundtrip failed\n");
      return false;
    }

    if (pacer_.frame() == before) {
      // Frame dropped (pool momentarily exhausted); the roundtrip should have
      // freed a buffer.  Bound the retries so a wedged compositor cannot spin
      // this loop forever.
      if (++stalls > kMaxStalls) {
        std::fprintf(stderr, "skia-shm-canvas: buffer pool stalled\n");
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
      "skia-shm-canvas: %zu frames  mean=%.3f ms  p50=%.3f  p95=%.3f  "
      "p99=%.3f\n",
      pacer_.sample_count(), pacer_.Mean(), pacer_.Percentile(50),
      pacer_.Percentile(95), pacer_.Percentile(99));
}

bool App::MainLoop() {
  if (pacer_.self_paced())
    return RunSelfPaced();

  std::printf("skia-shm-canvas: press ESC or Ctrl-C to quit\n");
  // g_running is cleared by the SIGINT handler; include it so the first Ctrl-C
  // exits cleanly.
  return wl::RunEventLoop(
      display_.Get(), [this] { return !running_ || !g_running; },
      "skia-shm-canvas");
}

}  // namespace

namespace {

void PrintUsage() {
  std::printf(
      "usage: skia_shm_canvas [--frames N] [--exit] [--fixed-dt]\n"
      "                       [--benchmark N]\n"
      "  --frames N     render at most N frames\n"
      "  --exit         quit once the frame limit is reached\n"
      "  --fixed-dt     deterministic 60 Hz animation clock\n"
      "  --benchmark N  render N frames self-paced and print frame-time "
      "stats\n");
}

[[nodiscard]] bool ParseArgs(const std::vector<std::string_view>& args,
                             demo::PacerConfig& cfg) {
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
    if (a == "--frames") {
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
  if (!ParseArgs(args, cfg)) {
    PrintUsage();
    return EXIT_FAILURE;
  }

  App app(cfg);
  return app.Run();
}
