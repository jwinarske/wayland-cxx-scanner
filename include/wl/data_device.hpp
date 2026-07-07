// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// data_device — header-only wl_data_device helper for the clipboard: reading
// the current selection (paste) and taking ownership of it (copy).  It binds
// wl_data_device_manager, gets the seat's data device, tracks the offered MIME
// types and hands the app a read fd on the paste side, and on the copy side
// creates a data source that serves the offered data on demand.
//
// This header must be included AFTER the generated wayland_client.hpp:
//   #include "wayland_client.hpp"   // defines CWlDataDevice, CWlDataOffer, …
//   #include <wl/data_device.hpp>   // wl::DataDevice<App>
//
// ── Optional App hooks (detected via SFINAE)
// ─────────────────────────────────────
//   void OnSelection(const wl::MimeSet& mimes);      // a selection arrived
//   void OnSend(const char* mime, wl::FdHandle fd);  // write the offered data
//   void OnCancelled();                              // the offer was dropped
//
// ── Lifecycle
// ─────────────────────────────────────────────────────────────────
//   1. Record(name, ver)   — from the registry OnGlobal callback for
//                            wl_data_device_manager (optional; no-op if
//                            absent).
//   2. Bind(registry, app) — after the registry roundtrip.
//   3. Start(display, seat) — once the display and seat proxy are available
//   (e.g. SeatManager::Seat()).
//   4. Paste: on a selection, the app calls Receive(mime) to get a read fd.
//      Copy:  the app calls Offer(mimes, serial) and produces data via OnSend.
//   5. Release()           — before member destructors run (App::~App).

#pragma once

#include <wl/client_helpers.hpp>  // wl::SetupHandler
#include <wl/fd_handle.hpp>       // wl::FdHandle
#include <wl/proxy_impl.hpp>      // wl::construct
#include <wl/registry.hpp>        // wl::CRegistry
#include <wl/wl_ptr.hpp>

extern "C" {
#include <fcntl.h>                // O_CLOEXEC
#include <unistd.h>               // pipe2, close
#include <wayland-client-core.h>  // wl_display, wl_display_flush
}

#include <algorithm>  // std::min
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>  // std::void_t
#include <utility>      // std::declval
#include <vector>

namespace wl {

// The MIME types offered for the current clipboard selection.  Populated once
// per selection from the wl_data_offer.offer events, so it is not a hot path.
class MimeSet {
 public:
  void Add(const char* mime) {
    if (mime != nullptr)
      items_.emplace_back(mime);
  }
  void Clear() noexcept { items_.clear(); }

  [[nodiscard]] bool Contains(std::string_view mime) const {
    return std::find(items_.begin(), items_.end(), mime) != items_.end();
  }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

  [[nodiscard]] auto begin() const noexcept { return items_.begin(); }
  [[nodiscard]] auto end() const noexcept { return items_.end(); }

 private:
  std::vector<std::string> items_;
};

namespace detail {

// True when App wants the clipboard selection: OnSelection(const MimeSet&).
template <typename A, typename = void>
struct HasDataSelection : std::false_type {};
template <typename A>
struct HasDataSelection<A,
                        std::void_t<decltype(std::declval<A&>().OnSelection(
                            std::declval<const MimeSet&>()))>>
    : std::true_type {};

// True when App accepts a clipboard send request: OnSend(mime, FdHandle).
template <typename A, typename = void>
struct HasDataSend : std::false_type {};
template <typename A>
struct HasDataSend<A,
                   std::void_t<decltype(std::declval<A&>().OnSend(
                       std::declval<const char*>(),
                       std::declval<wl::FdHandle>()))>> : std::true_type {};

// True when App wants notice that its offered selection was superseded.
template <typename A, typename = void>
struct HasDataCancelled : std::false_type {};
template <typename A>
struct HasDataCancelled<A,
                        std::void_t<decltype(std::declval<A&>().OnCancelled())>>
    : std::true_type {};

}  // namespace detail

// Protocol-traits bundle for the core wl_data_device family.  DataDevice is
// parameterized on a bundle like this so the one helper drives any
// structurally-isomorphic clipboard family (e.g. ext-data-control) from a
// different bundle — only the trait types, the CRTP bases, and two shape flags
// differ.
struct CoreDataProtocol {
  using ManagerTraits = wayland::client::wl_data_device_manager_traits;
  using DeviceTraits = wayland::client::wl_data_device_traits;
  using OfferTraits = wayland::client::wl_data_offer_traits;
  using SourceTraits = wayland::client::wl_data_source_traits;
  template <class D>
  using ManagerBase = wayland::client::CWlDataDeviceManager<D>;
  template <class D>
  using DeviceBase = wayland::client::CWlDataDevice<D>;
  template <class D>
  using OfferBase = wayland::client::CWlDataOffer<D>;
  template <class D>
  using SourceBase = wayland::client::CWlDataSource<D>;
  // set_selection carries a serial; release is a since-2 destructor request
  // (so it must be sent explicitly, version-gated, before the proxy is freed).
  static constexpr bool set_selection_takes_serial = true;
  static constexpr bool device_release_is_explicit = true;
};

// ══════════════════════════════════════════════════════════════════════════════
// wl::DataDevice<App, Protocol>
//
// Clipboard helper for a data-device family (core wl_data_device by default).
// Read side: a selection arrives as an offer introduced by the data_offer
// event, its MIME types stream in via offer events, and the selection event
// marks it current; the app reads a type through Receive().  Write side:
// Offer(mimes, serial) publishes a source the app serves via OnSend.
// ══════════════════════════════════════════════════════════════════════════════

template <typename App, typename P = CoreDataProtocol>
class DataDevice {
 public:
  DataDevice() noexcept = default;

  DataDevice(const DataDevice&) = delete;
  DataDevice& operator=(const DataDevice&) = delete;
  DataDevice(DataDevice&&) = delete;
  DataDevice& operator=(DataDevice&&) = delete;

  ~DataDevice() noexcept { Release(); }

  /// Record the wl_data_device_manager global for later binding.  Call from the
  /// registry OnGlobal callback.  The protocol is optional.
  void Record(std::uint32_t name, std::uint32_t ver) noexcept {
    name_ = name;
    ver_adv_ = ver;
  }

  /// Bind wl_data_device_manager.  No-op (returns true) when it was never
  /// advertised.  The manager has no events, so it is adopted without a
  /// listener.
  [[nodiscard]] bool Bind(wl::CRegistry& registry, App* app) noexcept {
    if (!name_)
      return true;  // optional
    using M = typename P::ManagerTraits;
    app_ = app;
    ver_ = std::min(ver_adv_, M::version);
    wl_proxy* raw = registry.Bind<M>(name_, ver_);
    if (raw == nullptr)
      return false;
    manager_.Attach(raw);
    return true;
  }

  /// Get the data device for @p seat and start listening for selections.  Call
  /// once the seat proxy is available (e.g. wl::SeatManager::Seat()).  @p
  /// display is retained so Receive() can flush the receive request.  No-op if
  /// the manager is not bound or the seat is null.
  void Start(wl_display* display, wl_proxy* seat) noexcept {
    if (manager_.IsNull() || seat == nullptr)
      return;
    display_ = display;
    using M = typename P::ManagerTraits;
    // get_data_device(new_id device, object seat): new_id is first, so plain
    // construct (nullptr, seat).
    wl_proxy* raw =
        wl::construct<typename P::DeviceTraits, M::Op::GetDataDevice>(
            *manager_.Get(), seat);
    if (wl::SetupHandler(device_, raw))
      device_.Get()->owner_ = this;
  }

  /// Open a pipe, ask the compositor to write the selection's @p mime data into
  /// it, and return the read end.  Returns an invalid FdHandle when there is no
  /// current selection, @p mime is null, or the pipe/flush fails.  The caller
  /// reads until EOF.
  [[nodiscard]] wl::FdHandle Receive(const char* mime) noexcept {
    if (offer_.IsNull() || mime == nullptr)
      return wl::FdHandle{-1};
    std::array<int, 2> fds{-1, -1};
    if (pipe2(fds.data(), O_CLOEXEC) != 0)
      return wl::FdHandle{-1};
    offer_.Get()->Receive(mime, fds[1]);
    close(fds[1]);  // the compositor holds its own copy via fd passing
    // Flush so the receive request reaches the compositor before we block on
    // the read end.
    if (display_ != nullptr)
      wl_display_flush(display_);
    return wl::FdHandle{fds[0]};
  }

  /// True when a selection offer is currently held.
  [[nodiscard]] bool HasSelection() const noexcept { return !offer_.IsNull(); }

  /// Take ownership of the clipboard: create a data source offering @p mimes
  /// and set it as the selection using @p serial from a real input event (e.g.
  /// wl::KeyEvent::serial or wl::PointerButtonEvent::serial).  The App produces
  /// the data on demand via OnSend and is told via OnCancelled when the offer
  /// is superseded.  No-op if the device has not been started.
  void Offer(const MimeSet& mimes,
             [[maybe_unused]] std::uint32_t serial) noexcept {
    if (manager_.IsNull() || device_.IsNull())
      return;
    source_.Reset();  // drop any previous offer we still own
    using M = typename P::ManagerTraits;
    wl_proxy* raw =
        wl::construct<typename P::SourceTraits, M::Op::CreateDataSource>(
            *manager_.Get());
    if (!wl::SetupHandler(source_, raw))
      return;
    source_.Get()->owner_ = this;
    for (const std::string& mime : mimes)
      source_.Get()->Offer(mime.c_str());
    if constexpr (P::set_selection_takes_serial)
      device_.Get()->SetSelection(source_.Get()->GetProxy(), serial);
    else
      device_.Get()->SetSelection(source_.Get()->GetProxy());
  }

  /// Send the versioned release and destroy the proxies.  Idempotent; call from
  /// App::~App before member destructors run.
  void Release() noexcept {
    source_.Reset();  // wl_data_source.destroy (destructor) via WlPtr
    offer_.Reset();   // wl_data_offer.destroy (destructor) via WlPtr
    ReleaseDevice();
    manager_.Reset();  // no destructor request — just frees the proxy
  }

 private:
  // ── Nested handlers
  // ──────────────────────────────────────────────────────────
  struct ManagerHandler : P::template ManagerBase<ManagerHandler> {};

  struct OfferHandler : P::template OfferBase<OfferHandler> {
    MimeSet mimes;
    void OnOffer(const char* mime) override { mimes.Add(mime); }
    // source_actions / action are DnD-only; the generated no-ops are fine.
  };

  struct DeviceHandler : P::template DeviceBase<DeviceHandler> {
    DataDevice* owner_ = nullptr;
    void OnDataOffer(wl_proxy* id) override {
      if (owner_ != nullptr)
        owner_->IntroduceOffer(id);
    }
    void OnSelection(wl_proxy* id) override {
      if (owner_ != nullptr)
        owner_->AdoptSelection(id);
    }
    // enter / leave / motion / drop are DnD-only; no-ops are fine.
  };

  struct SourceHandler : P::template SourceBase<SourceHandler> {
    DataDevice* owner_ = nullptr;
    void OnSend(const char* mime, std::int32_t fd) override {
      if (owner_ != nullptr)
        owner_->SendRequested(mime, fd);
    }
    void OnCancelled() override {
      if (owner_ != nullptr)
        owner_->SourceCancelled();
    }
    // target / dnd_drop_performed / dnd_finished / action are DnD-only.
  };

  // A new offer is being introduced; adopt it in place so its handler (and the
  // proxy's listener) accumulate the MIME types.  A single slot is correct for
  // the clipboard: each data_offer is immediately followed by its offer events
  // and the selection that references it.  (Promoting between two WlPtr slots
  // is impossible here — WlPtr::Swap moves only the proxy handle, not the
  // handler object that holds the accumulated MimeSet and owns the listener
  // binding.)
  void IntroduceOffer(wl_proxy* id) noexcept {
    offer_.Reset();
    (void)wl::SetupHandler(offer_, id);
  }

  // The selection now refers to @p id (the offer just introduced), or null to
  // clear it.  Deliver the offer's MIME set to the app.
  void AdoptSelection(wl_proxy* id) noexcept {
    if (id == nullptr) {
      offer_.Reset();
      const MimeSet empty;
      if (app_ != nullptr)
        CallSelection(empty);
      return;
    }
    if (!offer_.IsNull() && offer_.Get()->GetProxy() == id && app_ != nullptr)
      CallSelection(offer_.Get()->mimes);
  }

  // The compositor wants the offered data for @p mime written to @p fd (on
  // behalf of a pasting client).  Hand the App a FdHandle that closes the fd
  // when it returns; if the App has no OnSend hook, close it here so the reader
  // sees EOF rather than blocking.
  void SendRequested(const char* mime, int fd) noexcept {
    if constexpr (detail::HasDataSend<App>::value) {
      if (app_ != nullptr) {
        app_->OnSend(mime, wl::FdHandle{fd});
        return;
      }
    }
    close(fd);
  }

  // Our offered selection was superseded or cleared; drop the source and
  // notify.
  void SourceCancelled() noexcept {
    source_.Reset();
    if constexpr (detail::HasDataCancelled<App>::value) {
      if (app_ != nullptr)
        app_->OnCancelled();
    }
  }

  void ReleaseDevice() noexcept {
    if (device_.IsNull())
      return;
    // Core sends an explicit, version-gated release request; a family whose
    // device teardown is a plain destructor request lets WlPtr::Reset (below)
    // send it.
    if constexpr (P::device_release_is_explicit) {
      using D = typename P::DeviceTraits;
      if (ver_ >= D::Op::Since::Release)
        device_.Get()->Release();
    }
    device_.Reset();
  }

  // Deliver the selection to the App when it wants it.  The caller guarantees
  // app_ is non-null.  if constexpr (not a variadic fallback) so a MimeSet is
  // never passed through `...` — that would abort at runtime.
  void CallSelection(const MimeSet& m) noexcept {
    if constexpr (detail::HasDataSelection<App>::value)
      app_->OnSelection(m);
  }

  // ── Members
  // ──────────────────────────────────────────────────────────────────
  wl::WlPtr<ManagerHandler> manager_;
  wl::WlPtr<DeviceHandler> device_;
  wl::WlPtr<OfferHandler> offer_;    // the current selection's offer (paste)
  wl::WlPtr<SourceHandler> source_;  // the App's offered selection (copy)
  wl_display* display_ = nullptr;    // for flushing the receive request
  std::uint32_t name_ = 0;
  std::uint32_t ver_adv_ = 0;
  std::uint32_t ver_ = 0;
  App* app_ = nullptr;
};

}  // namespace wl
