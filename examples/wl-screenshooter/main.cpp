// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// wl-screenshooter — capture a Wayland compositor output to a PPM file.
//
// Modelled after libweston/screenshooter.c (Weston commit 544618d3c6):
//   • binds weston_screenshooter from the compositor registry
//   • allocates a wl_shm buffer sized to the first advertised output
//   • calls weston_screenshooter.shoot(output, buffer) and awaits done
//
// Access patterns from wlroots/wlr-clients screencopy-dmabuf.c:
//   • lambda-based CRegistry global scanner
//   • roundtrip after globals scan, roundtrip after output-mode settle
//   • dispatch loop until frame completion
//
// WTL pattern throughout:
//   • all protocol objects are CRTP handler classes held by wl::WlPtr<T>
//   • constructor requests use wl::construct<ChildTraits, Opcode>(parent, …)
//   • registry scanning uses wl::CRegistry with OnGlobal lambda
//   • graceful exit with a diagnostic message if any required global is absent

// ── Generated C++ protocol headers ───────────────────────────────────────────
// wayland_client.hpp     → namespace wayland::client  (from wayland.xml)
//                          provides: CWlOutput, CWlShm, CWlShmPool, CWlBuffer
// screenshooter_client.hpp → namespace weston_screenshooter::client
#include "screenshooter_client.hpp"
#include "wayland_client.hpp"

// ── System Wayland C headers ──────────────────────────────────────────────────
extern "C" {
// wl_*_interface symbols and WL_OUTPUT_MODE_CURRENT used by the wl_iface()
// definitions and pixel-format selection below.
#include <wayland-client-protocol.h>
// POSIX shared-memory helpers.
#include <fcntl.h>    // MFD_CLOEXEC (glibc exposes via fcntl.h or sys/mman.h)
#include <sys/mman.h>  // memfd_create, mmap, munmap, MAP_FAILED
#include <unistd.h>    // close, ftruncate
}

// ── Framework headers ─────────────────────────────────────────────────────────
#include <wl/raii.hpp>      // wl::ScopeExit
#include <wl/registry.hpp>  // wl::CRegistry
#include <wl/wl_ptr.hpp>    // wl::WlPtr<T>

// ── Standard library ──────────────────────────────────────────────────────────
#include <algorithm>  // std::min
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// <wayland-client-protocol.h> provides pre-built extern const wl_interface
// symbols for every core Wayland interface.  We delegate to them so that
// libwayland's type-checking machinery sees the canonical objects.
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

const wl_interface& wl_output_traits::wl_iface() noexcept {
  return wl_output_interface;
}
const wl_interface& wl_shm_traits::wl_iface() noexcept {
  return wl_shm_interface;
}
const wl_interface& wl_shm_pool_traits::wl_iface() noexcept {
  return wl_shm_pool_interface;
}
const wl_interface& wl_buffer_traits::wl_iface() noexcept {
  return wl_buffer_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// wl_interface definition for weston_screenshooter
//
// The shoot request takes two object arguments (wl_output, wl_buffer).
// Their interface pointers live in kShooterTypes[], which references the
// pre-built system symbols from <wayland-client-protocol.h>.
// ══════════════════════════════════════════════════════════════════════════════

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
//
// • avoid-non-const-global-variables / avoid-c-arrays: wl_message::types is
//   const wl_interface**; the element type must remain non-const to match.
// • interfaces-global-init: every initialiser is an address constant
//   (&wl_output_interface etc.) — link-time constants in C++ — so ordering
//   with respect to other TU-level objects is not a concern.

static const wl_interface* kShooterTypes[] = {
    &wl_output_interface,  // shoot → output  (arg 0)
    &wl_buffer_interface,  // shoot → buffer  (arg 1)
};

static const wl_message kShooterRequests[] = {
    {"shoot", "oo", kShooterTypes},
};

static const wl_message kShooterEvents[] = {
    {"done", "u", nullptr},
};

static const wl_interface kShooterIfaceDef = {
    "weston_screenshooter",
    1,
    1,
    kShooterRequests,
    1,
    kShooterEvents,
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

namespace weston_screenshooter::client {

const wl_interface& weston_screenshooter_traits::wl_iface() noexcept {
  return kShooterIfaceDef;
}

}  // namespace weston_screenshooter::client

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
//
// Each class inherits the generated CRTP base and overrides only the event
// methods it cares about.  The WTL pattern is:
//   • classes with events  → _SetProxy() installs the listener table
//   • classes without events → Attach() skips listener installation
// ══════════════════════════════════════════════════════════════════════════════

// ── WlOutputHandler ──────────────────────────────────────────────────────────
// Records the current-mode dimensions and signals when done (v2+).

class WlOutputHandler : public wayland::client::CWlOutput<WlOutputHandler> {
 public:
  int32_t width = 0;
  int32_t height = 0;
  bool mode_done = false;

  void OnGeometry(int32_t /*x*/,
                  int32_t /*y*/,
                  int32_t /*physical_width*/,
                  int32_t /*physical_height*/,
                  uint32_t /*subpixel*/,
                  const char* /*make*/,
                  const char* /*model*/,
                  uint32_t /*transform*/) override {}

  void OnMode(uint32_t flags,
              int32_t w,
              int32_t h,
              int32_t /*refresh*/) override {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
      width = w;
      height = h;
    }
  }

  // done fires (v2+) when all output properties for a cycle have been sent.
  void OnDone() override { mode_done = true; }

  void OnScale(int32_t /*factor*/) override {}
  void OnName(const char* /*name*/) override {}
  void OnDescription(const char* /*description*/) override {}
};

// ── WlShmHandler ─────────────────────────────────────────────────────────────
// Listens for supported pixel formats.  XRGB8888 is mandatory per the
// Wayland spec, but we verify it anyway.

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  bool has_xrgb8888 = false;

  void OnFormat(uint32_t format) override {
    if (format ==
        static_cast<uint32_t>(wayland::client::WlShmFormat::Xrgb8888)) {
      has_xrgb8888 = true;
    }
  }
};

// ── WlShmPoolHandler ─────────────────────────────────────────────────────────
// wl_shm_pool has no events.  Provide the required ProcessEvent stub so the
// class is concrete.  Use WlPtr::Attach() rather than _SetProxy() — no
// listener table is generated for event-free interfaces.

class WlShmPoolHandler
    : public wayland::client::CWlShmPool<WlShmPoolHandler> {
 public:
  bool ProcessEvent(uint32_t /*opcode*/, void** /*args*/) override {
    return false;
  }
};

// ── WlBufferHandler ──────────────────────────────────────────────────────────
// wl_buffer carries a single release event.  For a one-shot screenshooter
// the compositor sends the shot done event before releasing the buffer, so
// OnRelease() need not gate any state machine.

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  void OnRelease() override {}
};

// ── ScreenshooterHandler ─────────────────────────────────────────────────────
// Records the outcome of a weston_screenshooter.shoot() call.

class ScreenshooterHandler
    : public weston_screenshooter::client::CWestonScreenshooter<
          ScreenshooterHandler> {
 public:
  bool done = false;
  weston_screenshooter::client::WestonScreenshooterOutcome outcome{};

  void OnDone(uint32_t out) override {
    outcome = static_cast<
        weston_screenshooter::client::WestonScreenshooterOutcome>(out);
    done = true;
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run(const char* output_path);
  ~App();

  App() = default;
  App(const App&) = delete;
  App& operator=(const App&) = delete;

 private:
  // ── Member declaration order determines RAII destruction order.
  //    Declared first → destroyed last; declared last → destroyed first.
  //
  //    Destruction sequence (reverse of declaration order):
  //      shooter_ → buffer_ → pool_ → shm_mem_ → shm_ → output_ →
  //      registry_ → display_

  // Wayland display — destroyed last so all proxy operations remain valid.
  struct DisplayRaii {
    wl_display* d = nullptr;
    ~DisplayRaii() noexcept {
      if (d)
        wl_display_disconnect(d);
    }
  } display_;

  // Registry — destroyed before display_.
  wl::CRegistry registry_;

  // wl_output — CRTP handler receives mode/done events.
  wl::WlPtr<WlOutputHandler> output_;

  // wl_shm — CRTP handler receives format advertisements.
  wl::WlPtr<WlShmHandler> shm_;

  // wl_shm_pool — no events; pool_.Attach(raw) used instead of _SetProxy.
  wl::WlPtr<WlShmPoolHandler> pool_;

  // Shared-memory file and mapping — lifetime spans pool and buffer usage.
  struct ShmMem {
    int fd = -1;
    void* data = nullptr;
    int32_t size = 0;

    ~ShmMem() noexcept {
      if (data && data != MAP_FAILED)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
        munmap(data, static_cast<size_t>(size));
      if (fd >= 0)
        close(fd);
    }
    ShmMem() = default;
    ShmMem(const ShmMem&) = delete;
    ShmMem& operator=(const ShmMem&) = delete;
  } shm_mem_;

  // wl_buffer — CRTP handler receives release event.
  wl::WlPtr<WlBufferHandler> buffer_;

  // weston_screenshooter — CRTP handler receives done event.
  wl::WlPtr<ScreenshooterHandler> shooter_;

  // ── Global names / versions recorded during registry scan ─────────────────
  uint32_t output_name_ = 0;
  uint32_t output_ver_ = 0;
  uint32_t shm_name_ = 0;
  uint32_t shm_ver_ = 0;
  uint32_t shooter_name_ = 0;
  uint32_t shooter_ver_ = 0;

  // ── Internal pipeline steps ───────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool WaitOutputMode();
  bool CreateShmBuffer();
  bool TakeShot();
  bool SavePpm(const char* path);

  // ── WTL-pattern helper templates ─────────────────────────────────────────

  /// Attach @p raw to a WlPtr handler and install its event listener.
  /// Returns false (and does nothing) if @p raw is null.
  template <typename Handler>
  [[nodiscard]] bool SetupHandler(wl::WlPtr<Handler>& ptr,
                                  wl_proxy* raw) noexcept {
    if (!raw)
      return false;
    ptr.Get()->_SetProxy(raw);
    return true;
  }

  /// Bind a registry global capped to the version the traits were compiled
  /// against, then set up the CRTP handler.  Returns false on failure.
  template <typename Traits, typename Handler>
  [[nodiscard]] bool BindHandler(wl::WlPtr<Handler>& ptr,
                                 uint32_t name,
                                 uint32_t ver) noexcept {
    return SetupHandler(
        ptr, registry_.Bind<Traits>(name, std::min(ver, Traits::version)));
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  // Send wl_output.release (protocol version ≥ 3) before member destructors
  // fire, so the compositor can clean up server-side output tracking.
  if (!output_.IsNull()) {
    const uint32_t bound =
        std::min(output_ver_,
                 wayland::client::wl_output_traits::version);
    if (bound >= wayland::client::wl_output_traits::Op::Since::Release)
      output_.Get()->Release();  // sends release then nullifies the proxy
  }
  // Remaining resources are cleaned up in reverse declaration order:
  //   shooter_ → buffer_ → pool_ → shm_mem_ → shm_ → output_ →
  //   registry_ → display_
}

// ── ConnectDisplay ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  display_.d = wl_display_connect(nullptr);
  if (!display_.d) {
    std::fprintf(stderr, "wl-screenshooter: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals ───────────────────────────────────────────────────────────────
//
// Use wl::CRegistry with an OnGlobal lambda to collect the three globals we
// need — access pattern from wlroots/wlr-clients screencopy-dmabuf.c.
//
// Exits gracefully with a clear diagnostic if any required global is absent.

bool App::ScanGlobals() {
  if (!registry_.Create(display_.d)) {
    std::fprintf(stderr, "wl-screenshooter: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/,
                            uint32_t name,
                            std::string_view iface,
                            uint32_t ver) {
    using namespace wayland::client;
    using namespace weston_screenshooter::client;

    // Record only the first wl_output (primary display).
    if (iface == wl_output_traits::interface_name && !output_name_) {
      output_name_ = name;
      output_ver_ = ver;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == weston_screenshooter_traits::interface_name) {
      shooter_name_ = name;
      shooter_ver_ = ver;
    }
  });

  // One roundtrip collects all wl_registry.global advertisements.
  if (wl_display_roundtrip(display_.d) < 0) {
    std::fprintf(stderr,
                 "wl-screenshooter: roundtrip for globals failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  // Report every missing interface before returning so the user gets a
  // single, complete error message rather than one interface at a time.
  bool ok = true;

  if (!output_name_) {
    std::fprintf(stderr,
                 "wl-screenshooter: wl_output not advertised "
                 "— is a display connected?\n");
    ok = false;
  }
  if (!shm_name_) {
    std::fprintf(stderr, "wl-screenshooter: wl_shm not advertised\n");
    ok = false;
  }
  if (!shooter_name_) {
    std::fprintf(stderr,
                 "wl-screenshooter: weston_screenshooter not advertised "
                 "— is Weston running with the screenshooter plugin?\n");
    ok = false;
  }

  return ok;
}

// ── BindGlobals ───────────────────────────────────────────────────────────────
//
// Bind the three required globals.  Every handler that has events uses
// BindHandler<Traits> which calls SetupHandler → _SetProxy to install the
// static listener table.

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace weston_screenshooter::client;

  // wl_output — events: geometry, mode, done, scale, name, description
  if (!BindHandler<wl_output_traits>(output_, output_name_, output_ver_)) {
    std::fprintf(stderr, "wl-screenshooter: wl_output bind failed\n");
    return false;
  }

  // wl_shm — event: format
  if (!BindHandler<wl_shm_traits>(shm_, shm_name_, shm_ver_)) {
    std::fprintf(stderr, "wl-screenshooter: wl_shm bind failed\n");
    return false;
  }

  // weston_screenshooter — event: done
  if (!BindHandler<weston_screenshooter_traits>(shooter_, shooter_name_,
                                                shooter_ver_)) {
    std::fprintf(stderr,
                 "wl-screenshooter: weston_screenshooter bind failed\n");
    return false;
  }

  return true;
}

// ── WaitOutputMode ────────────────────────────────────────────────────────────
//
// After binding wl_output, the compositor sends geometry + mode (+ done for
// v2+) events.  A roundtrip ensures those are processed; if done (v2+) has
// not yet arrived we dispatch until it does.

bool App::WaitOutputMode() {
  if (wl_display_roundtrip(display_.d) < 0) {
    std::fprintf(stderr,
                 "wl-screenshooter: roundtrip after output bind failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  // If the compositor advertises wl_output v2+, wait for the done event which
  // signals that all output properties for this cycle have been delivered.
  const uint32_t bound =
      std::min(output_ver_, wayland::client::wl_output_traits::version);
  if (bound >= wayland::client::wl_output_traits::Evt::Since::Done) {
    while (!output_.Get()->mode_done) {
      if (wl_display_dispatch(display_.d) < 0) {
        std::fprintf(
            stderr,
            "wl-screenshooter: dispatch error waiting for output done: %s\n",
            std::strerror(errno));
        return false;
      }
    }
  }

  // Verify we received at least one current-mode event.
  if (output_.Get()->width <= 0 || output_.Get()->height <= 0) {
    std::fprintf(
        stderr,
        "wl-screenshooter: no current mode received from wl_output\n");
    return false;
  }

  // Verify the compositor supports the pixel format we are going to use.
  // (XRGB8888 is mandatory per the Wayland spec, but explicit is better.)
  if (!shm_.Get()->has_xrgb8888) {
    std::fprintf(stderr,
                 "wl-screenshooter: compositor does not advertise "
                 "WL_SHM_FORMAT_XRGB8888\n");
    return false;
  }

  std::printf("wl-screenshooter: output size %dx%d\n",
              output_.Get()->width, output_.Get()->height);
  return true;
}

// ── CreateShmBuffer ───────────────────────────────────────────────────────────
//
// Allocate an anonymous shared-memory file, map it, then create a wl_shm_pool
// and a wl_buffer from it using wl::construct<ChildTraits, Opcode>(parent, …)
// — the WTL-pattern type-safe constructor helper.

bool App::CreateShmBuffer() {
  const int32_t width = output_.Get()->width;
  const int32_t height = output_.Get()->height;
  const int32_t stride = width * 4;  // 4 bytes per pixel: XRGB8888
  const int32_t size = stride * height;

  // ── Shared-memory file ────────────────────────────────────────────────────
  // memfd_create() produces an anonymous, sealable file descriptor — the
  // recommended approach on Linux 3.17+ (and any kernel that runs Weston).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  shm_mem_.fd = memfd_create("wl-screenshooter", MFD_CLOEXEC);
  if (shm_mem_.fd < 0) {
    std::fprintf(stderr, "wl-screenshooter: memfd_create failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  if (ftruncate(shm_mem_.fd, static_cast<off_t>(size)) < 0) {
    std::fprintf(stderr, "wl-screenshooter: ftruncate failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  shm_mem_.data = mmap(nullptr, static_cast<size_t>(size),
                       PROT_READ | PROT_WRITE, MAP_SHARED, shm_mem_.fd, 0);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  if (shm_mem_.data == MAP_FAILED) {
    shm_mem_.data = nullptr;
    std::fprintf(stderr, "wl-screenshooter: mmap failed: %s\n",
                 std::strerror(errno));
    return false;
  }
  shm_mem_.size = size;

  // ── wl_shm_pool via wl::construct<> ──────────────────────────────────────
  // wl_shm.create_pool(new_id:wl_shm_pool, fd:fd, size:int32)
  wl_proxy* const pool_raw =
      wl::construct<wayland::client::wl_shm_pool_traits,
                    wayland::client::wl_shm_traits::Op::CreatePool>(
          *shm_.Get(), shm_mem_.fd, size);
  if (!pool_raw) {
    std::fprintf(stderr, "wl-screenshooter: wl_shm.create_pool failed\n");
    return false;
  }
  // WlShmPool has no events — use Attach() instead of _SetProxy().
  pool_.Attach(pool_raw);

  // ── wl_buffer via wl::construct<> ────────────────────────────────────────
  // wl_shm_pool.create_buffer(new_id:wl_buffer, offset, width, height,
  //                            stride, format)
  const uint32_t fmt =
      static_cast<uint32_t>(wayland::client::WlShmFormat::Xrgb8888);
  wl_proxy* const buf_raw =
      wl::construct<wayland::client::wl_buffer_traits,
                    wayland::client::wl_shm_pool_traits::Op::CreateBuffer>(
          *pool_.Get(), 0, width, height, stride, fmt);
  if (!buf_raw) {
    std::fprintf(stderr,
                 "wl-screenshooter: wl_shm_pool.create_buffer failed\n");
    return false;
  }
  buffer_.Get()->_SetProxy(buf_raw);

  wl_display_flush(display_.d);
  return true;
}

// ── TakeShot ──────────────────────────────────────────────────────────────────
//
// Dispatch the shoot() request and wait for the done event — access pattern
// from wlroots/wlr-clients screencopy-dmabuf.c.

bool App::TakeShot() {
  // weston_screenshooter.shoot(output, buffer) — WTL CRTP method.
  shooter_.Get()->Shoot(output_.Get()->GetProxy(), buffer_.Get()->GetProxy());
  wl_display_flush(display_.d);

  // Dispatch until the done event is delivered.
  while (!shooter_.Get()->done) {
    if (wl_display_dispatch(display_.d) < 0) {
      std::fprintf(stderr,
                   "wl-screenshooter: wl_display_dispatch error: %s\n",
                   std::strerror(errno));
      return false;
    }
  }

  using Outcome = weston_screenshooter::client::WestonScreenshooterOutcome;
  switch (shooter_.Get()->outcome) {
    case Outcome::Success:
      std::printf("wl-screenshooter: capture succeeded\n");
      return true;
    case Outcome::NoMemory:
      std::fprintf(
          stderr,
          "wl-screenshooter: compositor reported no_memory during capture\n");
      return false;
    case Outcome::BadBuffer:
      std::fprintf(stderr,
                   "wl-screenshooter: compositor reported bad_buffer "
                   "(dimensions or format mismatch)\n");
      return false;
    default:
      std::fprintf(stderr,
                   "wl-screenshooter: unknown outcome %u\n",
                   static_cast<uint32_t>(shooter_.Get()->outcome));
      return false;
  }
}

// ── SavePpm ───────────────────────────────────────────────────────────────────
//
// Write the captured pixels to a P6 (binary) PPM file.
//
// Pixel layout for WL_SHM_FORMAT_XRGB8888 on a little-endian machine:
//   byte 0: Blue, byte 1: Green, byte 2: Red, byte 3: padding (X)
// P6 PPM expects:  R, G, B  per pixel.

bool App::SavePpm(const char* path) {
  const int32_t width = output_.Get()->width;
  const int32_t height = output_.Get()->height;
  const int32_t stride = width * 4;

  FILE* const f = std::fopen(path, "wb");
  if (!f) {
    std::fprintf(stderr, "wl-screenshooter: cannot open %s: %s\n",
                 path, std::strerror(errno));
    return false;
  }
  const auto guard = wl::ScopeExit{[f] { std::fclose(f); }};

  std::fprintf(f, "P6\n%d %d\n255\n", width, height);

  const auto* pixels = static_cast<const uint8_t*>(shm_mem_.data);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t* px = pixels + (y * stride + x * 4);
      // XRGB8888 byte order: [B, G, R, X] → PPM RGB: [R, G, B]
      const uint8_t rgb[3] = {px[2], px[1], px[0]};
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
      if (std::fwrite(rgb, 1, 3, f) != 3) {
        std::fprintf(stderr, "wl-screenshooter: write error: %s\n",
                     std::strerror(errno));
        return false;
      }
    }
  }

  std::printf("wl-screenshooter: saved %dx%d screenshot to %s\n",
              width, height, path);
  return true;
}

// ── Run ───────────────────────────────────────────────────────────────────────

int App::Run(const char* output_path) {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  if (!WaitOutputMode())
    return EXIT_FAILURE;
  if (!CreateShmBuffer())
    return EXIT_FAILURE;
  if (!TakeShot())
    return EXIT_FAILURE;
  if (!SavePpm(output_path))
    return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
  const char* const output_path =
      (argc >= 2) ? argv[1] : "screenshot.ppm";  // NOLINT(*-pointer-arithmetic)
  App app;
  return app.Run(output_path);
}
