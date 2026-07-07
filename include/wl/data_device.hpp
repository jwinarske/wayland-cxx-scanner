// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// data_device — header-only wl_data_device helper for reading the clipboard
// selection (paste).  It binds wl_data_device_manager, gets the seat's data
// device, tracks the offered MIME types for the current selection, and hands
// the app a pipe fd to read the data from.  The copy/send side (which needs a
// serial from a real input event for set_selection) is intentionally not here.
//
// This header must be included AFTER the generated wayland_client.hpp:
//   #include "wayland_client.hpp"   // defines CWlDataDevice, CWlDataOffer, …
//   #include <wl/data_device.hpp>   // wl::DataDevice<App>
//
// ── Optional App hook (detected via SFINAE)
// ───────────────────────────────────
//   void OnSelection(const wl::MimeSet& mimes);  // clipboard changed
//
// ── Lifecycle
// ─────────────────────────────────────────────────────────────────
//   1. Record(name, ver)   — from the registry OnGlobal callback for
//                            wl_data_device_manager (optional; no-op if
//                            absent).
//   2. Bind(registry, app) — after the registry roundtrip.
//   3. Start(display, seat) — once the display and seat proxy are available
//   (e.g. SeatManager::Seat()).
//   4. On a selection, the app calls Receive(mime) to get a read fd.
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
#include <utility>  // std::declval
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

// ══════════════════════════════════════════════════════════════════════════════
// wl::DataDevice<App>
//
// Clipboard read side.  A selection arrives as a wl_data_offer introduced by
// the data_offer event; its MIME types stream in via offer events; the
// selection event marks it current.  The app is notified with the MIME set and
// reads the bytes for a chosen type through Receive().
// ══════════════════════════════════════════════════════════════════════════════

template <typename App>
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
    using M = wayland::client::wl_data_device_manager_traits;
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
    using M = wayland::client::wl_data_device_manager_traits;
    // get_data_device(new_id device, object seat): new_id is first, so plain
    // construct (nullptr, seat).
    wl_proxy* raw = wl::construct<wayland::client::wl_data_device_traits,
                                  M::Op::GetDataDevice>(*manager_.Get(), seat);
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

  /// Send the versioned release and destroy the proxies.  Idempotent; call from
  /// App::~App before member destructors run.
  void Release() noexcept {
    offer_.Reset();  // wl_data_offer.destroy (destructor) via WlPtr
    ReleaseDevice();
    manager_.Reset();  // no destructor request — just frees the proxy
  }

 private:
  // ── Nested handlers
  // ──────────────────────────────────────────────────────────
  struct ManagerHandler
      : wayland::client::CWlDataDeviceManager<ManagerHandler> {};

  struct OfferHandler : wayland::client::CWlDataOffer<OfferHandler> {
    MimeSet mimes;
    void OnOffer(const char* mime) override { mimes.Add(mime); }
    // source_actions / action are DnD-only; the generated no-ops are fine.
  };

  struct DeviceHandler : wayland::client::CWlDataDevice<DeviceHandler> {
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

  void ReleaseDevice() noexcept {
    if (device_.IsNull())
      return;
    using D = wayland::client::wl_data_device_traits;
    if (ver_ >= D::Op::Since::Release)
      device_.Get()->Release();  // release request (since v2) + destroys proxy
    device_.Reset();
  }

  // ── Optional App hook (SFINAE, mirroring PointerHandler)
  // ─────────────────────
  template <typename A = App>
  auto CallSelection(const MimeSet& m)
      -> decltype(std::declval<A&>().OnSelection(m), void()) {
    app_->OnSelection(m);
  }
  void CallSelection(...) noexcept {}

  // ── Members
  // ──────────────────────────────────────────────────────────────────
  wl::WlPtr<ManagerHandler> manager_;
  wl::WlPtr<DeviceHandler> device_;
  wl::WlPtr<OfferHandler> offer_;  // the current selection's offer
  wl_display* display_ = nullptr;  // for flushing the receive request
  std::uint32_t name_ = 0;
  std::uint32_t ver_adv_ = 0;
  std::uint32_t ver_ = 0;
  App* app_ = nullptr;
};

}  // namespace wl
