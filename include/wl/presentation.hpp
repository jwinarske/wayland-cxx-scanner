// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// presentation — header-only wp_presentation manager that turns per-commit
// wp_presentation_feedback into typed, allocation-free timing events.  It is a
// peer of wl::SeatManager: the example binds it from the registry, arms one
// feedback per frame just before wl_surface.commit, and receives the
// compositor's real turn-to-light timestamp back through optional App hooks.
//
// ── Include order
// ─────────────────────────────────────────────────────────────
// This header must be included AFTER the generated presentation-time client
// header, which is generated with --emit-interface-tables so it already
// supplies the wl_interface tables and wl_iface() definitions (this header does
// NOT redefine them):
//
//   #include "presentation_time_client.hpp"  // presentation_time::client
//   #include <wl/presentation.hpp>           // wl::PresentationManager<App>
//
// ── Optional App hooks (detected via SFINAE)
// ────────────────────────────────── The App implements whichever it wants;
// both are optional:
//
//   void OnPresented(const wl::PresentFeedback& fb);  // frame turned to light
//   void OnDiscarded(std::uint32_t frame);            // frame never shown
//
// ── Lifecycle
// ─────────────────────────────────────────────────────────────────
//   1. Record(name, ver)   — from the registry OnGlobal callback for
//                            wp_presentation (optional; no-op if never seen).
//   2. Bind(registry, app) — after the registry roundtrip.
//   3. Arm(surface, frame) — immediately BEFORE each wl_surface.commit.
//   4. Release()           — before member destructors run (App::~App), or let
//                            the destructor's safety net handle it.

#pragma once

#include <wl/client_helpers.hpp>  // wl::BindHandler, wl::SetupHandler
#include <wl/present_feedback.hpp>  // wl::PresentFeedback, detail::DecodePresented
#include <wl/proxy_impl.hpp>        // wl::construct_at_end
#include <wl/registry.hpp>          // wl::CRegistry
#include <wl/wl_ptr.hpp>

extern "C" {
#include <time.h>  // clock_gettime, clockid_t, timespec, CLOCK_MONOTONIC
}

#include <algorithm>  // std::min
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>  // std::declval

namespace wl {

// ══════════════════════════════════════════════════════════════════════════════
// wl::PresentationManager<App>
//
// Owns wp_presentation and a fixed pool of in-flight feedback objects.  Single
// threaded by design: the example's event loop dispatches the feedback events,
// so no locking is needed (unlike a vsync source that marshals across threads).
// ══════════════════════════════════════════════════════════════════════════════

template <typename App>
class PresentationManager {
 public:
  // Feedback is short-lived (the compositor answers within a frame or two), so
  // a small fixed array bounds outstanding commits and keeps the steady state
  // free of per-frame heap allocation.
  static constexpr std::size_t kMaxInFlight = 8;

  // Plausibility window for the reported refresh interval: a virtual or hostile
  // compositor could send 0 or an absurd value.  Clamp to [1000 Hz, 10 Hz];
  // anything outside is reported as 0 ("unknown") rather than trusted.
  static constexpr std::uint32_t kMinRefreshNs = 1'000'000u;    // 1000 Hz
  static constexpr std::uint32_t kMaxRefreshNs = 100'000'000u;  // 10 Hz

  PresentationManager() noexcept = default;

  // Non-copyable, non-movable (owns wl_proxy* resources).
  PresentationManager(const PresentationManager&) = delete;
  PresentationManager& operator=(const PresentationManager&) = delete;
  PresentationManager(PresentationManager&&) = delete;
  PresentationManager& operator=(PresentationManager&&) = delete;

  ~PresentationManager() noexcept { Release(); }

  /// Record the wp_presentation global for later binding.  Call from the
  /// registry OnGlobal callback.  wp_presentation is optional.
  void Record(std::uint32_t name, std::uint32_t ver) noexcept {
    name_ = name;
    ver_adv_ = ver;
  }

  /// Bind wp_presentation and install the clock_id handler.
  ///
  /// No-op (returns true) when Record() was never called — the protocol is
  /// optional.  Returns false only when the global was advertised but the bind
  /// failed.
  [[nodiscard]] bool Bind(wl::CRegistry& registry, App* app) noexcept {
    if (!name_)
      return true;  // wp_presentation not advertised — optional, not an error
    using P = presentation_time::client::wp_presentation_traits;
    app_ = app;
    // Cap at v2 (adds the zero-copy feedback flag); no v2-only request is used.
    const std::uint32_t ver = std::min({ver_adv_, P::version, 2u});
    if (!wl::BindHandler<P>(registry, presentation_, name_, ver))
      return false;
    presentation_.Get()->mgr_ = this;
    return true;
  }

  [[nodiscard]] bool Bound() const noexcept { return !presentation_.IsNull(); }

  /// The compositor's presentation clock (CLOCK_MONOTONIC until clock_id
  /// fires).
  [[nodiscard]] clockid_t clock() const noexcept { return clk_; }

  /// Request presentation feedback for the frame about to be committed.
  ///
  /// Stamps the commit time on the presentation clock so OnPresented can report
  /// commit→photons latency.  MUST be called immediately BEFORE
  /// wl_surface.commit.  No-op when wp_presentation is absent, the in-flight
  /// pool is saturated, or the request fails — feedback is best-effort.
  void Arm(wl_proxy* surface, std::uint32_t frame) noexcept {
    if (presentation_.IsNull() || surface == nullptr)
      return;
    Reap();  // free slots whose feedback already fired, before reusing one
    const std::size_t idx = FreeSlot();
    if (idx == kMaxInFlight)
      return;  // pool saturated — drop this frame's feedback (bounded)

    using FB = presentation_time::client::wp_presentation_feedback_traits;
    using P = presentation_time::client::wp_presentation_traits;
    // feedback(surface, callback:new_id): the new_id is the TRAILING wire arg,
    // so construct_at_end (not construct, which emits new_id first).
    wl_proxy* raw = wl::construct_at_end<FB, P::Op::Feedback>(
        *presentation_.Get(), surface);
    Slot& s = slots_.at(idx);
    if (!wl::SetupHandler(s.fb, raw))
      return;
    s.fb.Get()->mgr_ = this;
    s.fb.Get()->slot_ = idx;
    s.commit_ns = NowNs();
    s.frame = frame;
    s.state = Slot::State::Live;
  }

  /// Destroy the proxies of feedback objects that have already fired.  Called
  /// from Arm(); also safe to call once per event-loop iteration.  Destroying
  /// here (rather than inside the firing event) keeps proxy teardown outside
  /// libwayland's dispatch, matching the wl_callback idiom.
  void Reap() noexcept {
    for (Slot& s : slots_) {
      if (s.state == Slot::State::Done) {
        s.fb.Reset();
        s.state = Slot::State::Free;
      }
    }
  }

  /// Destroy all feedback proxies and wp_presentation.  Idempotent.  Call from
  /// App::~App before member destructors run; the destructor also calls it.
  void Release() noexcept {
    for (Slot& s : slots_) {
      s.fb.Reset();
      s.state = Slot::State::Free;
    }
    presentation_.Reset();
  }

 private:
  // ── Internal handlers
  // ───────────────────────────────────────────────────────
  struct PresentationHandler
      : presentation_time::client::CWpPresentation<PresentationHandler> {
    PresentationManager* mgr_ = nullptr;
    void OnClockId(std::uint32_t id) override {
      if (mgr_ != nullptr)
        mgr_->clk_ = static_cast<clockid_t>(id);
    }
  };

  struct FeedbackHandler
      : presentation_time::client::CWpPresentationFeedback<FeedbackHandler> {
    PresentationManager* mgr_ = nullptr;
    std::size_t slot_ = 0;
    void OnSyncOutput(wl_proxy* /*output*/) override {}
    void OnPresented(std::uint32_t tv_sec_hi,
                     std::uint32_t tv_sec_lo,
                     std::uint32_t tv_nsec,
                     std::uint32_t refresh,
                     std::uint32_t seq_hi,
                     std::uint32_t seq_lo,
                     std::uint32_t flags) override {
      if (mgr_ != nullptr)
        mgr_->Presented(slot_, tv_sec_hi, tv_sec_lo, tv_nsec, refresh, seq_hi,
                        seq_lo, flags);
    }
    void OnDiscarded() override {
      if (mgr_ != nullptr)
        mgr_->Discarded(slot_);
    }
  };

  struct Slot {
    enum class State { Free, Live, Done };
    wl::WlPtr<FeedbackHandler> fb;
    double commit_ns = 0.0;
    std::uint32_t frame = 0;
    State state = State::Free;
  };

  [[nodiscard]] std::size_t FreeSlot() noexcept {
    for (std::size_t i = 0; i < kMaxInFlight; ++i)
      if (slots_.at(i).state == Slot::State::Free)
        return i;
    return kMaxInFlight;
  }

  [[nodiscard]] double NowNs() const noexcept {
    timespec ts{};
    clock_gettime(clk_, &ts);
    return static_cast<double>(ts.tv_sec) * 1.0e9 +
           static_cast<double>(ts.tv_nsec);
  }

  void Presented(std::size_t slot,
                 std::uint32_t tv_sec_hi,
                 std::uint32_t tv_sec_lo,
                 std::uint32_t tv_nsec,
                 std::uint32_t refresh,
                 std::uint32_t seq_hi,
                 std::uint32_t seq_lo,
                 std::uint32_t flags) noexcept {
    Slot& s = slots_.at(slot);
    s.state = Slot::State::Done;
    const std::optional<PresentFeedback> fb = wl::detail::DecodePresented(
        s.frame, s.commit_ns, tv_sec_hi, tv_sec_lo, tv_nsec, refresh, seq_hi,
        seq_lo, flags, kMinRefreshNs, kMaxRefreshNs);
    if (app_ == nullptr)
      return;
    // nullopt means the timestamp was unusable (overflow guard) — report it as
    // a discard so the App still sees the frame resolve.
    if (fb)
      CallPresented(*fb);
    else
      CallDiscarded(s.frame);
  }

  void Discarded(std::size_t slot) noexcept {
    Slot& s = slots_.at(slot);
    s.state = Slot::State::Done;
    if (app_ != nullptr)
      CallDiscarded(s.frame);
  }

  // ── Optional App hooks (detected via SFINAE, mirroring PointerHandler)
  // ───────
  template <typename A = App>
  auto CallPresented(const PresentFeedback& f)
      -> decltype(std::declval<A&>().OnPresented(f), void()) {
    app_->OnPresented(f);
  }
  void CallPresented(...) noexcept {}

  template <typename A = App>
  auto CallDiscarded(std::uint32_t frame)
      -> decltype(std::declval<A&>().OnDiscarded(frame), void()) {
    app_->OnDiscarded(frame);
  }
  void CallDiscarded(...) noexcept {}

  // ── Members
  // ──────────────────────────────────────────────────────────────────
  wl::WlPtr<PresentationHandler> presentation_;
  std::array<Slot, kMaxInFlight> slots_{};
  clockid_t clk_ = CLOCK_MONOTONIC;
  std::uint32_t name_ = 0;     // global id recorded during the registry scan
  std::uint32_t ver_adv_ = 0;  // advertised version
  App* app_ = nullptr;
};

}  // namespace wl
