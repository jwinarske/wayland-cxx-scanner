// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// dmabuf_feedback — header-only helper for zwp_linux_dmabuf_v1 feedback
// (v4/v5). It binds zwp_linux_dmabuf_v1, requests default or per-surface
// feedback, and accumulates the streamed events into a value-semantic
// FeedbackSnapshot (main device + tranches of resolved format/modifier pairs)
// that the app queries for scanout-capable modifiers.  On a v1-v3 compositor
// (no feedback object) it synthesizes an equivalent single-tranche snapshot
// from the legacy format/modifier events, so every consumer writes one code
// path.
//
// This header must be included AFTER the generated linux_dmabuf_client.hpp and
// the wl_interface tables:
//   #include "linux_dmabuf_client.hpp"  // defines CZwpLinuxDmabufV1, …
//   #include <wl/linux_dmabuf.hpp>      // tables + wl_iface() impls
//   #include <wl/dmabuf_feedback.hpp>   // wl::DmabufFeedback<App>
//
// ── Optional App hooks (detected via SFINAE)
// ─────────────────────────────────────
//   void OnDmabufFeedback(const wl::FeedbackSnapshot&)  - fired on every `done`
//        (initial advertisement and every re-advertisement).  The reference is
//        owned by the helper and valid until the next `done` or Release(); copy
//        it for longer retention (FeedbackSnapshot is a plain value type).
//   void OnDmabufLegacyFormat(uint32_t fourcc, uint64_t modifier)  - a v1-v3
//        ladder rung; most consumers omit it and use the synthesized snapshot.
//
// ── Lifecycle
// ─────────────────────────────────────────────────────────────────
//   1. Record(name, ver)     — from the registry OnGlobal callback for
//                              zwp_linux_dmabuf_v1 (optional; no-op if absent).
//   2. Bind(registry, app)   — after the registry roundtrip.  Binds
//                              min(advertised, 5).
//   3. StartDefault(display) / StartSurface(display, wl_surface) — create the
//      feedback object (needs >= 4).  On a v3-or-lower compositor these return
//      false; call CommitLegacy() after a roundtrip to publish the synthesized
//      snapshot from the format/modifier events instead.
//   4. Current()             — the latest committed snapshot.
//   5. Release()             — before member destructors run (App::~App).
//
// ── Threading
// ─────────────────────────────────────────────────────────────────
//   Single-threaded with the dispatching wl_display, like every wl:: helper;
//   not defended with locks.

#pragma once

#include <wl/client_helpers.hpp>  // wl::SetupHandler
#include <wl/proxy_impl.hpp>      // wl::construct
#include <wl/registry.hpp>        // wl::CRegistry
#include <wl/wl_ptr.hpp>

extern "C" {
#include <sys/mman.h>             // mmap, munmap
#include <sys/types.h>            // dev_t
#include <unistd.h>               // close
#include <wayland-client-core.h>  // wl_display, wl_array
}

#include <algorithm>  // std::min
#include <cstdint>
#include <cstring>  // std::memcpy
#include <type_traits>
#include <utility>  // std::declval, std::move
#include <vector>

namespace wl {

// A DRM fourcc paired with a DRM format modifier.
struct FormatModifier {
  std::uint32_t format = 0;
  std::uint64_t modifier = 0;
};

// One feedback tranche: the modifiers usable for a device/flags class, resolved
// from the format table.  A tranche with the scanout flag set lists modifiers
// the compositor may promote directly onto a hardware plane.
struct FeedbackTranche {
  dev_t target_device = 0;  // 0 = not sent (reuse main device)
  std::uint32_t flags = 0;  // raw bitfield; bit 0 = scanout
  std::vector<FormatModifier> formats;

  [[nodiscard]] bool Scanout() const noexcept { return (flags & 1u) != 0u; }
};

// A committed feedback advertisement: the main device plus its tranches, in the
// order the compositor sent them (preference order).  Plain value type — copy
// it freely; the query helpers are linear scans over what are, in practice,
// a handful of tranches of a few hundred entries.
struct FeedbackSnapshot {
  dev_t main_device = 0;
  std::vector<FeedbackTranche> tranches;

  // Modifiers advertised for @p fourcc across all tranches, tranche order
  // preserved (a modifier can appear once per tranche it is listed in).
  [[nodiscard]] std::vector<std::uint64_t> ModifiersFor(
      std::uint32_t fourcc) const {
    std::vector<std::uint64_t> out;
    for (const FeedbackTranche& t : tranches)
      for (const FormatModifier& fm : t.formats)
        if (fm.format == fourcc)
          out.push_back(fm.modifier);
    return out;
  }

  // Modifiers for @p fourcc from scanout tranches only, tranche order
  // preserved — the compositor's plane-promotable candidates.
  [[nodiscard]] std::vector<std::uint64_t> ScanoutModifiersFor(
      std::uint32_t fourcc) const {
    std::vector<std::uint64_t> out;
    for (const FeedbackTranche& t : tranches)
      if (t.Scanout())
        for (const FormatModifier& fm : t.formats)
          if (fm.format == fourcc)
            out.push_back(fm.modifier);
    return out;
  }

  // True if @p fourcc / @p modifier appears in any tranche.
  [[nodiscard]] bool Supports(std::uint32_t fourcc,
                              std::uint64_t modifier) const {
    for (const FeedbackTranche& t : tranches)
      for (const FormatModifier& fm : t.formats)
        if (fm.format == fourcc && fm.modifier == modifier)
          return true;
    return false;
  }
};

namespace detail {

// True when App wants each committed snapshot: OnDmabufFeedback(const
// FeedbackSnapshot&).
template <typename A, typename = void>
struct HasDmabufFeedback : std::false_type {};
template <typename A>
struct HasDmabufFeedback<
    A,
    std::void_t<decltype(std::declval<A&>().OnDmabufFeedback(
        std::declval<const FeedbackSnapshot&>()))>> : std::true_type {};

// True when App wants the raw legacy events: OnDmabufLegacyFormat(fourcc, mod).
template <typename A, typename = void>
struct HasDmabufLegacyFormat : std::false_type {};
template <typename A>
struct HasDmabufLegacyFormat<
    A,
    std::void_t<decltype(std::declval<A&>().OnDmabufLegacyFormat(
        std::declval<std::uint32_t>(),
        std::declval<std::uint64_t>()))>> : std::true_type {};

}  // namespace detail

// ══════════════════════════════════════════════════════════════════════════════
// wl::DmabufFeedback<App>
//
// One instance manages one feedback object (default or per-surface).  Bind()
// binds the dmabuf global; StartDefault/StartSurface create the feedback and
// begin accumulating; Current() is the latest committed snapshot.
// ══════════════════════════════════════════════════════════════════════════════

template <typename App>
class DmabufFeedback {
  using DmabufTraits =
      linux_dmabuf_unstable_v1::client::zwp_linux_dmabuf_v1_traits;
  using FeedbackTraits =
      linux_dmabuf_unstable_v1::client::zwp_linux_dmabuf_feedback_v1_traits;

 public:
  // DRM_FORMAT_MOD_INVALID, defined locally so the header pulls in no libdrm.
  static constexpr std::uint64_t kModifierInvalid = 0x00ffffffffffffffULL;

  DmabufFeedback() noexcept = default;
  ~DmabufFeedback() noexcept { Release(); }

  DmabufFeedback(const DmabufFeedback&) = delete;
  DmabufFeedback& operator=(const DmabufFeedback&) = delete;
  DmabufFeedback(DmabufFeedback&&) = delete;
  DmabufFeedback& operator=(DmabufFeedback&&) = delete;

  // Record the zwp_linux_dmabuf_v1 global for later binding.  Call from the
  // registry OnGlobal callback.  The protocol is optional.
  void Record(std::uint32_t name, std::uint32_t ver) noexcept {
    name_ = name;
    ver_adv_ = ver;
  }

  // Bind zwp_linux_dmabuf_v1 at min(advertised, 5).  Returns true (no-op) when
  // it was never advertised; false only when the bind itself fails.  Installs
  // the dispatcher so legacy format/modifier events (v1-v3) reach the helper.
  [[nodiscard]] bool Bind(wl::CRegistry& registry, App* app) noexcept {
    if (name_ == 0)
      return true;  // optional
    app_ = app;
    ver_ = std::min(ver_adv_, DmabufTraits::version);
    wl_proxy* raw = registry.Bind<DmabufTraits>(name_, ver_);
    if (!wl::SetupHandler(dmabuf_, raw))
      return false;
    dmabuf_.Get()->owner_ = this;
    return true;
  }

  // Create a zwp_linux_buffer_params_v1 (create_params) on the bound dmabuf,
  // for building a dma-buf-backed wl_buffer.  Returns the new params proxy (the
  // caller owns and drives add/create_immed), or null when the dmabuf is not
  // bound.  The dmabuf owner is the natural place to mint a params object.
  [[nodiscard]] wl_proxy* CreateParams() noexcept {
    if (dmabuf_.IsNull())
      return nullptr;
    return wl::construct<
        linux_dmabuf_unstable_v1::client::zwp_linux_buffer_params_v1_traits,
        DmabufTraits::Op::CreateParams>(*dmabuf_.Get());
  }

  // Request default feedback.  Requires a bound v4+ dmabuf; returns false
  // otherwise (use the legacy path — CommitLegacy — on older compositors).
  [[nodiscard]] bool StartDefault(wl_display* display) noexcept {
    return Start(display, nullptr);
  }

  // Request per-surface feedback for @p surface.  Requires a bound v4+ dmabuf
  // and a non-null surface; returns false otherwise.
  [[nodiscard]] bool StartSurface(wl_display* display,
                                  wl_proxy* surface) noexcept {
    return surface != nullptr && Start(display, surface);
  }

  // Publish the snapshot synthesized from legacy format/modifier events as if a
  // `done` had arrived (there is no protocol `done` at v1-v3).  A no-op at v4+.
  // Call once after the roundtrip that drained the dmabuf global's events.
  void CommitLegacy() noexcept {
    if (ver_ >= 4)
      return;
    current_ = FeedbackSnapshot{};
    current_.tranches.push_back(std::move(legacy_tranche_));
    legacy_tranche_ = FeedbackTranche{};
    FireFeedback();
  }

  [[nodiscard]] std::uint32_t BoundVersion() const noexcept { return ver_; }
  [[nodiscard]] const FeedbackSnapshot& Current() const noexcept {
    return current_;
  }
  // Lifetime count of committed snapshots (advertisements).  Useful in tests.
  [[nodiscard]] std::uint64_t commit_count() const noexcept {
    return commit_count_;
  }

  // Destroy the feedback and dmabuf proxies.  Idempotent; call from App::~App
  // before member destructors run.
  void Release() noexcept {
    feedback_.Reset();  // zwp_linux_dmabuf_feedback_v1.destroy (destructor)
    dmabuf_.Reset();    // zwp_linux_dmabuf_v1.destroy (destructor)
  }

 private:
  // ── Nested proxy handlers
  // ───────────────────────────────────────────────────
  struct DmabufHandler
      : linux_dmabuf_unstable_v1::client::CZwpLinuxDmabufV1<DmabufHandler> {
    DmabufFeedback* owner_ = nullptr;
    void OnFormat(std::uint32_t format) override {
      if (owner_ != nullptr)
        owner_->LegacyFormat(format, kModifierInvalid);
    }
    void OnModifier(std::uint32_t format,
                    std::uint32_t hi,
                    std::uint32_t lo) override {
      if (owner_ != nullptr)
        owner_->LegacyFormat(format,
                             (static_cast<std::uint64_t>(hi) << 32u) | lo);
    }
  };

  struct FeedbackHandler
      : linux_dmabuf_unstable_v1::client::CZwpLinuxDmabufFeedbackV1<
            FeedbackHandler> {
    DmabufFeedback* owner_ = nullptr;
    void OnDone() override {
      if (owner_ != nullptr)
        owner_->Done();
    }
    void OnFormatTable(std::int32_t fd, std::uint32_t size) override {
      if (owner_ != nullptr)
        owner_->FormatTable(fd, size);
    }
    void OnMainDevice(wl_array* device) override {
      if (owner_ != nullptr)
        owner_->MainDevice(device);
    }
    void OnTrancheDone() override {
      if (owner_ != nullptr)
        owner_->TrancheDone();
    }
    void OnTrancheTargetDevice(wl_array* device) override {
      if (owner_ != nullptr)
        owner_->TrancheTargetDevice(device);
    }
    void OnTrancheFormats(wl_array* indices) override {
      if (owner_ != nullptr)
        owner_->TrancheFormats(indices);
    }
    void OnTrancheFlags(std::uint32_t flags) override {
      if (owner_ != nullptr)
        owner_->pending_tranche_.flags = flags;
    }
  };

  // Shared entry point for StartDefault/StartSurface.
  [[nodiscard]] bool Start(wl_display* display, wl_proxy* surface) noexcept {
    if (dmabuf_.IsNull() || ver_ < 4)
      return false;
    display_ = display;
    wl_proxy* raw = surface == nullptr
                        ? wl::construct<FeedbackTraits,
                                        DmabufTraits::Op::GetDefaultFeedback>(
                              *dmabuf_.Get())
                        : wl::construct<FeedbackTraits,
                                        DmabufTraits::Op::GetSurfaceFeedback>(
                              *dmabuf_.Get(), surface);
    if (!wl::SetupHandler(feedback_, raw))
      return false;
    feedback_.Get()->owner_ = this;
    return true;
  }

  // ── Feedback event accumulation (pending → committed on `done`)
  // ──────────────
  void FormatTable(std::int32_t fd, std::uint32_t size) noexcept {
    table_.clear();
    // Guard against a compositor bug: the table is 16-byte entries.  Truncate
    // to the largest valid multiple rather than trusting the reported size.
    const std::uint32_t usable = size - (size % kEntrySize);
    if (usable != 0) {
      void* map = ::mmap(nullptr, usable, PROT_READ, MAP_PRIVATE, fd, 0);
      if (map != MAP_FAILED) {
        const std::uint32_t count = usable / kEntrySize;
        table_.reserve(count);
        const auto* bytes = static_cast<const unsigned char*>(map);
        for (std::uint32_t i = 0; i < count; ++i) {
          RawEntry e{};
          std::memcpy(&e, bytes + static_cast<std::size_t>(i) * kEntrySize,
                      kEntrySize);
          table_.push_back(FormatModifier{e.format, e.modifier});
        }
        ::munmap(map, usable);
      }
    }
    ::close(fd);
  }

  void MainDevice(wl_array* device) noexcept {
    pending_main_ = DevFromArray(device);
  }
  void TrancheTargetDevice(wl_array* device) noexcept {
    pending_tranche_.target_device = DevFromArray(device);
  }
  void TrancheFormats(wl_array* indices) noexcept {
    if (indices == nullptr || indices->data == nullptr)
      return;
    const auto* idx = static_cast<const std::uint16_t*>(indices->data);
    const std::size_t n = indices->size / sizeof(std::uint16_t);
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint16_t k = idx[i];
      if (k < table_.size())  // out-of-range index dropped (compositor bug)
        pending_tranche_.formats.push_back(table_[k]);
    }
  }
  void TrancheDone() noexcept {
    pending_tranches_.push_back(std::move(pending_tranche_));
    pending_tranche_ = FeedbackTranche{};
  }
  void Done() noexcept {
    current_.main_device = pending_main_;
    current_.tranches = std::move(pending_tranches_);
    pending_tranches_.clear();
    pending_tranche_ = FeedbackTranche{};
    pending_main_ = 0;
    // The format table persists across `done` for re-advertisement reuse.
    FireFeedback();
  }

  // ── Legacy (v1-v3) synthesis
  // ────────────────────────────────────────────────
  void LegacyFormat(std::uint32_t format, std::uint64_t modifier) noexcept {
    legacy_tranche_.formats.push_back(FormatModifier{format, modifier});
    if constexpr (detail::HasDmabufLegacyFormat<App>::value)
      if (app_ != nullptr)
        app_->OnDmabufLegacyFormat(format, modifier);
  }

  void FireFeedback() noexcept {
    ++commit_count_;
    if constexpr (detail::HasDmabufFeedback<App>::value)
      if (app_ != nullptr)
        app_->OnDmabufFeedback(current_);
  }

  // Read a dev_t out of a wl_array without assuming its size equals sizeof.
  static dev_t DevFromArray(wl_array* a) noexcept {
    dev_t dev = 0;
    if (a != nullptr && a->data != nullptr)
      std::memcpy(&dev, a->data, std::min(sizeof(dev), a->size));
    return dev;
  }

  // 16-byte format-table entry: {u32 format, u32 pad, u64 modifier}.
  struct RawEntry {
    std::uint32_t format;
    std::uint32_t pad;
    std::uint64_t modifier;
  };
  static constexpr std::uint32_t kEntrySize = 16;

  // ── Members
  // ─────────────────────────────────────────────────────────────────
  wl::WlPtr<DmabufHandler> dmabuf_;
  wl::WlPtr<FeedbackHandler> feedback_;
  wl_display* display_ = nullptr;

  std::vector<FormatModifier> table_;  // format table copy (retained)
  dev_t pending_main_ = 0;
  FeedbackTranche pending_tranche_;
  std::vector<FeedbackTranche> pending_tranches_;
  FeedbackTranche legacy_tranche_;  // v1-v3 synthesis accumulator

  FeedbackSnapshot current_;
  std::uint64_t commit_count_ = 0;

  std::uint32_t name_ = 0;
  std::uint32_t ver_adv_ = 0;
  std::uint32_t ver_ = 0;
  App* app_ = nullptr;
};

}  // namespace wl
