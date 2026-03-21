// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// sdl3-presentation-shm — SDL3 window with wp_presentation frame-timing
//
// Demonstrates using wayland-cxx-scanner generated C++ bindings for an
// external Wayland protocol (presentation-time) alongside SDL3.  SDL3
// owns the window, renderer, and Wayland surface; we extract the raw
// wl_display and wl_surface via SDL3 properties and bind wp_presentation
// ourselves for accurate per-frame timing feedback.
//
// The spinning color wheel is rendered into an SDL_Texture each frame
// and presented via SDL_RenderPresent().  Presentation feedback is
// printed to stdout.
//
// Three run modes (selected by command-line flag):
//   -f  feedback      (default) continuous rendering; prints c2p, p2p per
//   frame. -i  feedback-idle           same but sleeps 1 s between frames. -p
//   low-lat present         render as fast as possible (no SDL_Delay pacing).
//
// Additional options:
//   -d MSECS   emulate rendering cost by sleeping MSECS before each commit.
//   -w WIDTH   window width  (default 250)
//   -h HEIGHT  window height (default 250)
//
// Usage:
//   sdl3_presentation_shm [-f|-i|-p] [-d MSECS] [-w WIDTH] [-h HEIGHT]

// ── Generated C++ protocol header ───────────────────────────────────────────
#include "presentation_time_client.hpp"  // namespace presentation_time::client

// ── Framework headers ───────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

// ── System Wayland C headers ────────────────────────────────────────────────
extern "C" {
#include <wayland-client-protocol.h>
}

// ── SDL3 ────────────────────────────────────────────────────────────────────
#include <SDL3/SDL.h>

// ── Standard library ────────────────────────────────────────────────────────
#include <array>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// Run mode
// ══════════════════════════════════════════════════════════════════════════════

enum class RunMode { Feedback, FeedbackIdle, LowLatPresent };

static constexpr std::string_view run_mode_name(const RunMode m) noexcept {
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
// Timing utilities
// ══════════════════════════════════════════════════════════════════════════════

static void timespec_from_proto(timespec& ts,
                                const uint32_t sec_hi,
                                const uint32_t sec_lo,
                                const uint32_t nsec) noexcept {
  ts.tv_sec =
      (static_cast<int64_t>(sec_hi) << 32) | static_cast<int64_t>(sec_lo);
  ts.tv_nsec = static_cast<long>(nsec);
}

static uint32_t timespec_to_ms(const timespec& ts) noexcept {
  return static_cast<uint32_t>(static_cast<uint64_t>(ts.tv_sec) * 1000u +
                               static_cast<uint64_t>(ts.tv_nsec / 1'000'000L));
}

static int64_t timespec_diff_us(const timespec& a, const timespec& b) noexcept {
  return (a.tv_sec - b.tv_sec) * 1'000'000LL + (a.tv_nsec - b.tv_nsec) / 1000LL;
}

// ══════════════════════════════════════════════════════════════════════════════
// Pixel painting — spinning color wheel (identical to presentation-shm)
// ══════════════════════════════════════════════════════════════════════════════

static void paint_pixels(std::span<uint32_t> buf,
                         const int width,
                         const int height,
                         const uint32_t phase) noexcept {
  const int halfh = height / 2;
  const int halfw = width / 2;

  const double ang =
      std::numbers::pi * 2.0 / 1'000'000.0 * static_cast<double>(phase);
  const double s = std::sin(ang);
  const double c = std::cos(ang);

  int64_t outer_r = (halfw < halfh ? halfw : halfh) - 16;
  outer_r *= outer_r;

  for (int y = 0; y < height; ++y) {
    const int oy = y - halfh;
    const int64_t y2 = static_cast<int64_t>(oy) * oy;

    for (int x = 0; x < width; ++x) {
      const int ox = x - halfw;
      const std::size_t idx =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);

      if (static_cast<int64_t>(ox) * ox + y2 > outer_r) {
        buf[idx] = (ox * oy > 0) ? 0xFF000000u : 0xFFFFFFFFu;
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

      buf[idx] = v;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

class App;

// ── WpPresentationHandler ───────────────────────────────────────────────────

class WpPresentationHandler
    : public presentation_time::client::CWpPresentation<WpPresentationHandler> {
 public:
  clockid_t clk_id = CLOCK_MONOTONIC;
  void OnClockId(const uint32_t id) override {
    clk_id = static_cast<clockid_t>(id);
  }
};

// ── WpPresentationFeedbackHandler ───────────────────────────────────────────

class WpPresentationFeedbackHandler
    : public presentation_time::client::CWpPresentationFeedback<
          WpPresentationFeedbackHandler> {
 public:
  App* app_ = nullptr;
  unsigned frame_no = 0;
  timespec commit{};
  uint32_t frame_stamp = 0;

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
// App class
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  App(const RunMode mode,
      const int commit_delay_ms,
      const int width,
      const int height)
      : mode_(mode),
        commit_delay_ms_(commit_delay_ms),
        width_(width),
        height_(height) {}
  ~App();

  int Run();

  void OnFeedbackPresented(WpPresentationFeedbackHandler& fb,
                           uint32_t tv_sec_hi,
                           uint32_t tv_sec_lo,
                           uint32_t tv_nsec,
                           uint32_t refresh_ns,
                           uint32_t seq_hi,
                           uint32_t seq_lo,
                           uint32_t flags) noexcept;
  void OnFeedbackDiscarded(const WpPresentationFeedbackHandler& fb) noexcept;

 private:
  // ── Configuration ────────────────────────────────────────────────────────
  RunMode mode_;
  int commit_delay_ms_ = 0;
  int width_ = 250;
  int height_ = 250;

  // ── SDL3 objects ──────────────────────────────────────────────────────────
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;

  // ── Wayland objects (obtained from SDL3) ──────────────────────────────────
  wl_display* wl_display_ = nullptr;
  wl_proxy* wl_surface_ = nullptr;

  wl::CRegistry registry_;
  wl::WlPtr<WpPresentationHandler> presentation_;
  bool have_presentation_ = false;
  uint32_t presentation_name_ = 0;
  uint32_t presentation_ver_ = 0;

  // ── Application state ─────────────────────────────────────────────────────
  bool running_ = true;
  unsigned frame_seq_ = 0;

  // Vsync pacing: frame_ready_ gates the new frame submission in Feedback
  // modes. Set to false after each commit, set back to true when the compositor
  // reports `presented` or `discarded`.  refresh_ns_ is updated from feedback.
  bool frame_ready_ = true;
  uint32_t refresh_ns_ = 0;  // set from display mode; updated by feedback
  uint64_t last_commit_ticks_ = 0;  // SDL_GetTicksNS() at last commit

  std::list<std::unique_ptr<WpPresentationFeedbackHandler>> feedback_list_;
  std::unique_ptr<WpPresentationFeedbackHandler> last_presented_;

  // Pixel buffer for CPU rendering into the texture.
  std::vector<uint32_t> pixels_;

  // ── Pipeline ──────────────────────────────────────────────────────────────
  bool InitSDL();
  bool InitPresentation();
  void AttachPresentationFeedback() noexcept;
  void EmulateRendering() const noexcept;
  std::unique_ptr<WpPresentationFeedbackHandler> ExtractFeedback(
      const WpPresentationFeedbackHandler& fb);
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

void WpPresentationFeedbackHandler::OnPresented(const uint32_t tv_sec_hi,
                                                const uint32_t tv_sec_lo,
                                                const uint32_t tv_nsec,
                                                const uint32_t refresh_ns,
                                                const uint32_t seq_hi,
                                                const uint32_t seq_lo,
                                                const uint32_t flags) {
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
  // Destroy all Wayland proxies BEFORE SDL_Quit() disconnects the display.
  // Feedback handlers are unique_ptr<T> (not WlPtr), so we must explicitly
  // destroy each proxy to unregister the listener — otherwise SDL's internal
  // Wayland dispatch during shutdown could fire a callback on freed memory.
  for (auto& fb : feedback_list_) {
    if (fb && !fb->IsNull())
      fb->Destroy();
  }
  feedback_list_.clear();
  if (last_presented_ && !last_presented_->IsNull())
    last_presented_->Destroy();
  last_presented_.reset();
  presentation_.Reset();
  registry_.Reset();

  if (texture_)
    SDL_DestroyTexture(texture_);
  if (renderer_)
    SDL_DestroyRenderer(renderer_);
  if (window_)
    SDL_DestroyWindow(window_);
  SDL_Quit();
}

bool App::InitSDL() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "sdl3-presentation-shm: SDL_Init: %s\n",
                 SDL_GetError());
    return false;
  }

  std::array<char, 128> title{};
  std::snprintf(title.data(), title.size(),
                "sdl3-presentation-shm: %.*s [delay %d ms]",
                static_cast<int>(run_mode_name(mode_).size()),
                run_mode_name(mode_).data(), commit_delay_ms_);
  window_ = SDL_CreateWindow(title.data(), width_, height_, 0);
  if (!window_) {
    std::fprintf(stderr, "sdl3-presentation-shm: SDL_CreateWindow: %s\n",
                 SDL_GetError());
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_) {
    std::fprintf(stderr, "sdl3-presentation-shm: SDL_CreateRenderer: %s\n",
                 SDL_GetError());
    return false;
  }

  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888,
                               SDL_TEXTUREACCESS_STREAMING, width_, height_);
  if (!texture_) {
    std::fprintf(stderr, "sdl3-presentation-shm: SDL_CreateTexture: %s\n",
                 SDL_GetError());
    return false;
  }

  pixels_.resize(static_cast<std::size_t>(width_) *
                 static_cast<std::size_t>(height_));

  // ── Extract Wayland objects from SDL3 ───────────────────────────────────
  const SDL_PropertiesID win_props = SDL_GetWindowProperties(window_);
  wl_display_ = static_cast<wl_display*>(SDL_GetPointerProperty(
      win_props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
  wl_surface_ = static_cast<wl_proxy*>(SDL_GetPointerProperty(
      win_props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));

  if (!wl_display_ || !wl_surface_) {
    std::fprintf(stderr,
                 "sdl3-presentation-shm: not running on Wayland — "
                 "presentation feedback unavailable\n");
    // Continue without presentation feedback; SDL rendering still works.
    return true;
  }

  // ── Query output refresh rate from the display mode ────────────────────
  if (const SDL_DisplayID display_id = SDL_GetDisplayForWindow(window_);
      display_id != 0) {
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display_id)) {
      if (mode->refresh_rate > 0.0f) {
        refresh_ns_ =
            static_cast<uint32_t>(1'000'000'000.0f / mode->refresh_rate);
        std::printf("sdl3-presentation-shm: display refresh=%.2f Hz (%u ns)\n",
                    static_cast<double>(mode->refresh_rate), refresh_ns_);
      }
    }
  }
  if (refresh_ns_ == 0) {
    refresh_ns_ = 16'666'667u;  // fallback to 60 Hz
    std::fprintf(stderr,
                 "sdl3-presentation-shm: could not query display refresh — "
                 "assuming 60 Hz\n");
  }

  return true;
}

bool App::InitPresentation() {
  if (!wl_display_)
    return true;  // not on Wayland, skip

  if (!registry_.Create(wl_display_)) {
    std::fprintf(stderr,
                 "sdl3-presentation-shm: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry&, const uint32_t name,
                            const std::string_view iface, const uint32_t ver) {
    using namespace presentation_time::client;
    if (iface == wp_presentation_traits::interface_name) {
      presentation_name_ = name;
      presentation_ver_ = ver;
    }
  });

  if (!wl::RoundtripWithTimeout(wl_display_)) {
    std::fprintf(stderr, "sdl3-presentation-shm: timed out scanning globals\n");
    return false;
  }

  if (presentation_name_) {
    using namespace presentation_time::client;
    if (wl::BindHandler<wp_presentation_traits>(
            registry_, presentation_, presentation_name_, presentation_ver_)) {
      have_presentation_ = true;
    }
  }

  if (!have_presentation_) {
    std::fprintf(stderr,
                 "sdl3-presentation-shm: wp_presentation not available — "
                 "timing feedback disabled\n");
  } else {
    // Roundtrip to receive clock_id.
    if (!wl::RoundtripWithTimeout(wl_display_)) {
      std::fprintf(stderr,
                   "sdl3-presentation-shm: timed out waiting for clock_id\n");
      return false;
    }
    std::printf("sdl3-presentation-shm: presentation clock_id=%d\n",
                static_cast<int>(presentation_.Get()->clk_id));
  }

  return true;
}

void App::AttachPresentationFeedback() noexcept {
  if (!have_presentation_)
    return;

  // Guard against unbounded growth if the compositor stops responding.
  if (feedback_list_.size() >= 16)
    return;

  using namespace presentation_time::client;

  auto* pres = presentation_.Get();

  auto fb = std::make_unique<WpPresentationFeedbackHandler>();
  fb->app_ = this;
  fb->frame_no = ++frame_seq_;
  if (clock_gettime(pres->clk_id, &fb->commit) != 0)
    fb->commit = {};

  if (wl_proxy* raw =
          wl::construct_at_end<wp_presentation_feedback_traits,
                               wp_presentation_traits::Op::Feedback>(
              *pres, wl_surface_)) {
    fb->_SetProxy(raw);
    feedback_list_.push_back(std::move(fb));
  }
}

void App::EmulateRendering() const noexcept {
  if (commit_delay_ms_ <= 0)
    return;
  SDL_Delay(static_cast<uint32_t>(commit_delay_ms_));
}

std::unique_ptr<WpPresentationFeedbackHandler> App::ExtractFeedback(
    const WpPresentationFeedbackHandler& fb) {
  const auto it = std::ranges::find_if(
      feedback_list_, [&fb](const auto& p) { return p.get() == &fb; });
  if (it == feedback_list_.end())
    return nullptr;
  auto ptr = std::move(*it);
  feedback_list_.erase(it);
  return ptr;
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

  const uint64_t seq =
      (static_cast<uint64_t>(seq_hi) << 32) | static_cast<uint64_t>(seq_lo);

  const uint32_t commit_ms = timespec_to_ms(fb.commit);
  const uint32_t present_ms = timespec_to_ms(present);
  const uint32_t c2p = present_ms - commit_ms;

  const timespec* prev_present =
      last_presented_ ? &last_presented_->commit : &present;
  const int64_t p2p = timespec_diff_us(present, *prev_present);

  std::array<char, 8> flagstr{"____"};
  if (flags & 0x1u)
    flagstr.at(0) = 's';  // vsync
  if (flags & 0x2u)
    flagstr.at(1) = 'c';  // hw_clock
  if (flags & 0x4u)
    flagstr.at(2) = 'e';  // hw_completion
  if (flags & 0x8u)
    flagstr.at(3) = 'z';  // zero_copy

  std::printf("%6u: c2p %4u ms, p2p %5" PRId64
              " us, refresh %u ns, "
              "[%s] seq %" PRIu64 "\n",
              fb.frame_no, c2p, p2p, refresh_ns, flagstr.data(), seq);
  std::fflush(stdout);

  if (refresh_ns > 0)
    refresh_ns_ = refresh_ns;
  frame_ready_ = true;

  fb.commit = present;
  last_presented_ = ExtractFeedback(fb);
}

void App::OnFeedbackDiscarded(
    const WpPresentationFeedbackHandler& fb) noexcept {
  std::printf("discarded %u\n", fb.frame_no);
  frame_ready_ = true;
  ExtractFeedback(fb);
}

int App::Run() {
  if (!InitSDL())
    return EXIT_FAILURE;
  if (!InitPresentation())
    return EXIT_FAILURE;

  std::printf(
      "sdl3-presentation-shm: mode=%.*s delay=%d ms "
      "(close window or press ESC to quit)\n",
      static_cast<int>(run_mode_name(mode_).size()),
      run_mode_name(mode_).data(), commit_delay_ms_);

  uint32_t phase = 0;
  while (running_) {
    // ── SDL event pump ────────────────────────────────────────────────────
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
        case SDL_EVENT_QUIT:
          running_ = false;
          break;
        case SDL_EVENT_KEY_DOWN:
          if (ev.key.key == SDLK_ESCAPE)
            running_ = false;
          break;
        default:
          break;
      }
    }

    if (!running_)
      break;

    // ── Dispatch pending Wayland events (feedback callbacks) ──────────────
    if (wl_display_) {
      wl_display_flush(wl_display_);
      wl_display_dispatch_pending(wl_display_);
    }

    // ── Vsync pacing (Feedback / FeedbackIdle modes) ─────────────────────
    // In Feedback modes we wait for the compositor to report that the
    // previous frame was presented (or discarded) before submitting the
    // next one.  This naturally paces to the display refresh rate and
    // avoids overwhelming the compositor with frames it would discard.
    //
    // A timeout based on 2× the refresh period prevents stalling if
    // feedback is lost or wp_presentation is unavailable.
    //
    // LowLatPresent skips the gate entirely — render as fast as possible.
    if (mode_ != RunMode::LowLatPresent && have_presentation_ &&
        !frame_ready_) {
      const uint64_t now = SDL_GetTicksNS();
      const uint64_t elapsed = now - last_commit_ticks_;
      if (const auto timeout_ns = static_cast<uint64_t>(refresh_ns_) * 2u;
          elapsed < timeout_ns) {
        // Yield briefly so we don't busy-loop; 1 ms is well within a
        // single refresh period and keeps the event loop responsive.
        SDL_Delay(1);
        continue;
      }
      // Timeout expired — compositor may not be sending feedback.
      // Fall through and render anyway to avoid stalling.
      frame_ready_ = true;
    }

    // ── Mode-specific delay ──────────────────────────────────────────────
    if (mode_ == RunMode::FeedbackIdle)
      SDL_Delay(1000);

    EmulateRendering();

    // ── Render the spinning wheel ─────────────────────────────────────────
    paint_pixels(pixels_, width_, height_, phase);
    phase += 10'000u;

    if (!SDL_UpdateTexture(texture_, nullptr, pixels_.data(),
                           static_cast<int>(static_cast<std::size_t>(width_) *
                                            sizeof(uint32_t)))) {
      std::fprintf(stderr, "sdl3-presentation-shm: SDL_UpdateTexture: %s\n",
                   SDL_GetError());
      return EXIT_FAILURE;
    }
    if (!SDL_RenderTexture(renderer_, texture_, nullptr, nullptr)) {
      std::fprintf(stderr, "sdl3-presentation-shm: SDL_RenderTexture: %s\n",
                   SDL_GetError());
      return EXIT_FAILURE;
    }

    // ── Attach presentation feedback BEFORE the SDL present ───────────────
    // SDL_RenderPresent() calls wl_surface_commit() internally, so we must
    // attach the feedback object before that commit happens.
    AttachPresentationFeedback();

    if (!SDL_RenderPresent(renderer_)) {
      std::fprintf(stderr, "sdl3-presentation-shm: SDL_RenderPresent: %s\n",
                   SDL_GetError());
      return EXIT_FAILURE;
    }

    // Mark the frame in-flight for pacing; record commit time for timeout.
    if (have_presentation_) {
      frame_ready_ = false;
      last_commit_ticks_ = SDL_GetTicksNS();
    }
  }

  std::fprintf(stderr, "sdl3-presentation-shm exiting\n");
  return EXIT_SUCCESS;
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

static void print_usage(const char* prog) {
  std::fprintf(stderr,
               "Usage: %s [mode] [options]\n"
               "where 'mode' is one of\n"
               "  -f  feedback (default)\n"
               "  -i  feedback-idle (sleep 1s between frames)\n"
               "  -p  low-latency present mode\n"
               "and 'options' may include\n"
               "  -d MSECS   emulate rendering cost with a sleep\n"
               "  -w WIDTH   window width  (default 250)\n"
               "  -h HEIGHT  window height (default 250)\n",
               prog);
}

int main(const int argc, char* argv[]) {
  const std::vector<std::string_view> args(argv, std::next(argv, argc));

  auto mode = RunMode::Feedback;
  int commit_delay_ms = 0;
  int width = 250;
  int height = 250;

  // Helper: parse a non-negative integer argument for the given option flag.
  const auto parse_int_arg = [&](const char* flag,
                                 const std::string_view val_str, int& out,
                                 const long min_val = 0) -> bool {
    char* end = nullptr;
    errno = 0;
    const long val = std::strtol(val_str.data(), &end, 10);
    if (errno == ERANGE || end == val_str.data() || *end != '\0' ||
        val < min_val || val > INT_MAX) {
      std::fprintf(stderr, "sdl3-presentation-shm: invalid %s value '%.*s'\n",
                   flag, static_cast<int>(val_str.size()), val_str.data());
      print_usage(args.at(0).data());
      return false;
    }
    out = static_cast<int>(val);
    return true;
  };

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (const auto& arg = args.at(i); arg == "-f")
      mode = RunMode::Feedback;
    else if (arg == "-i")
      mode = RunMode::FeedbackIdle;
    else if (arg == "-p")
      mode = RunMode::LowLatPresent;
    else if (arg == "-d" && i + 1 < args.size()) {
      if (!parse_int_arg("-d", args.at(++i), commit_delay_ms))
        return EXIT_FAILURE;
    } else if (arg == "-w" && i + 1 < args.size()) {
      if (!parse_int_arg("-w", args.at(++i), width, 1))
        return EXIT_FAILURE;
    } else if (arg == "-h" && i + 1 < args.size()) {
      if (!parse_int_arg("-h", args.at(++i), height, 1))
        return EXIT_FAILURE;
    } else {
      print_usage(args.at(0).data());
      return EXIT_FAILURE;
    }
  }

  App app{mode, commit_delay_ms, width, height};
  return app.Run();
}
