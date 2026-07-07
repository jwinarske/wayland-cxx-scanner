// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Live wl_display dispatch test for wl::DataDevice<App> (clipboard paste).
//
// A hand-rolled libwayland server plays the compositor: it advertises wl_seat
// and wl_data_device_manager, and on get_data_device it introduces a
// wl_data_offer, streams its MIME types, and marks it the selection.  The
// client side is the generated proxies driven by wl::DataDevice — it must
// receive the MIME set through OnSelection and read the exact bytes the server
// writes into the receive() pipe.  Exercises the event-created-object path
// (data_offer new_id) end to end.  Run under ASan+UBSan.

#include "dd_client.hpp"  // client-header, --emit-interface-tables

#include <wl/data_device.hpp>
#include <wl/registry.hpp>

extern "C" {
#include <unistd.h>  // read, write, close
#include <wayland-client-core.h>
#include <wayland-server-core.h>
}

#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace wc = wayland::client;

namespace {

constexpr std::string_view kPayload = "clipboard-hello";

const wl_interface* SeatIface() {
  return &wc::wl_seat_traits::wl_iface();
}
const wl_interface* ManagerIface() {
  return &wc::wl_data_device_manager_traits::wl_iface();
}
const wl_interface* DeviceIface() {
  return &wc::wl_data_device_traits::wl_iface();
}
const wl_interface* OfferIface() {
  return &wc::wl_data_offer_traits::wl_iface();
}
const wl_interface* SourceIface() {
  return &wc::wl_data_source_traits::wl_iface();
}

// ── Server side: raw libwayland acting as the compositor ─────────────────────

void ServerReceive(wl_client* /*client*/,
                   wl_resource* /*offer*/,
                   const char* /*mime*/,
                   int32_t fd) {
  const ssize_t n = write(fd, kPayload.data(), kPayload.size());
  (void)n;
  close(fd);
}
void ServerDestroyResource(wl_client* /*client*/, wl_resource* resource) {
  wl_resource_destroy(resource);
}

// wl_data_offer requests: accept, receive, destroy, finish, set_actions.
struct OfferImpl {
  void (*accept)(wl_client*, wl_resource*, uint32_t, const char*);
  void (*receive)(wl_client*, wl_resource*, const char*, int32_t);
  void (*destroy)(wl_client*, wl_resource*);
  void (*finish)(wl_client*, wl_resource*);
  void (*set_actions)(wl_client*, wl_resource*, uint32_t, uint32_t);
};
constexpr OfferImpl kOfferImpl{nullptr, &ServerReceive, &ServerDestroyResource,
                               nullptr, nullptr};

// wl_data_device requests: start_drag, set_selection, release.
struct DeviceImpl {
  void (*start_drag)(wl_client*,
                     wl_resource*,
                     wl_resource*,
                     wl_resource*,
                     wl_resource*,
                     uint32_t);
  void (*set_selection)(wl_client*, wl_resource*, wl_resource*, uint32_t);
  void (*release)(wl_client*, wl_resource*);
};
constexpr DeviceImpl kDeviceImpl{nullptr, nullptr, &ServerDestroyResource};

void ServerGetDataDevice(wl_client* client,
                         wl_resource* mgr,
                         uint32_t id,
                         wl_resource* /*seat*/) {
  const int ver = wl_resource_get_version(mgr);
  wl_resource* device =
      wl_resource_create(client, DeviceIface(), ver, static_cast<int>(id));
  wl_resource_set_implementation(device, &kDeviceImpl, nullptr, nullptr);

  // Introduce an offer, stream its MIME types, then make it the selection.
  wl_resource* offer = wl_resource_create(client, OfferIface(), ver, 0);
  wl_resource_set_implementation(offer, &kOfferImpl, nullptr, nullptr);
  wl_resource_post_event(device, wc::wl_data_device_traits::Evt::DataOffer,
                         offer);
  wl_resource_post_event(offer, wc::wl_data_offer_traits::Evt::Offer,
                         "text/plain");
  wl_resource_post_event(offer, wc::wl_data_offer_traits::Evt::Offer,
                         "text/html");
  wl_resource_post_event(device, wc::wl_data_device_traits::Evt::Selection,
                         offer);
}

// wl_data_device_manager requests: create_data_source, get_data_device.
struct ManagerImpl {
  void (*create_data_source)(wl_client*, wl_resource*, uint32_t);
  void (*get_data_device)(wl_client*, wl_resource*, uint32_t, wl_resource*);
};
constexpr ManagerImpl kManagerImpl{nullptr, &ServerGetDataDevice};

void ManagerBind(wl_client* client,
                 void* /*data*/,
                 uint32_t version,
                 uint32_t id) {
  wl_resource* r = wl_resource_create(
      client, ManagerIface(), static_cast<int>(version), static_cast<int>(id));
  wl_resource_set_implementation(r, &kManagerImpl, nullptr, nullptr);
}
void SeatBind(wl_client* client,
              void* /*data*/,
              uint32_t version,
              uint32_t id) {
  // The seat is only needed as the get_data_device argument; make no requests
  // of it and send no events (the client sets no listener on it).
  wl_resource_create(client, SeatIface(), static_cast<int>(version),
                     static_cast<int>(id));
}

// ── Client side: the App driving wl::DataDevice ──────────────────────────────

struct App {
  wl::DataDevice<App> data_device;
  wl::MimeSet mimes;
  bool got_selection = false;
  void OnSelection(const wl::MimeSet& m) {
    got_selection = true;
    mimes = m;
  }
};

TEST(DataDeviceRoundtrip, ReceivesSelectionMimesAndReadsPayload) {
  std::array<int, 2> sv{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  wl_display* server = wl_display_create();
  ASSERT_NE(server, nullptr);
  ASSERT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server, SeatIface(), 1, nullptr, &SeatBind);
  wl_global_create(server, ManagerIface(), 3, nullptr, &ManagerBind);
  std::thread server_thread([server] { wl_display_run(server); });

  wl_display* client = wl_display_connect_to_fd(sv[1]);
  ASSERT_NE(client, nullptr);

  {
    App app;
    wl_proxy* seat = nullptr;
    uint32_t seat_name = 0;
    uint32_t seat_ver = 0;

    wl::CRegistry registry;
    ASSERT_TRUE(registry.Create(client));
    registry.OnGlobal([&](wl::CRegistry&, uint32_t n, std::string_view iface,
                          uint32_t v) {
      if (iface == wc::wl_seat_traits::interface_name) {
        seat_name = n;
        seat_ver = v;
      } else if (iface == wc::wl_data_device_manager_traits::interface_name) {
        app.data_device.Record(n, v);
      }
    });
    ASSERT_NE(wl_display_roundtrip(client), -1);
    ASSERT_NE(seat_name, 0u);

    seat = registry.Bind<wc::wl_seat_traits>(
        seat_name, std::min(seat_ver, wc::wl_seat_traits::version));
    ASSERT_NE(seat, nullptr);
    ASSERT_TRUE(app.data_device.Bind(registry, &app));
    app.data_device.Start(client, seat);

    ASSERT_NE(wl_display_roundtrip(client), -1);

    // The selection arrived with both offered MIME types.
    EXPECT_TRUE(app.got_selection);
    EXPECT_TRUE(app.mimes.Contains("text/plain"));
    EXPECT_TRUE(app.mimes.Contains("text/html"));
    EXPECT_EQ(app.mimes.size(), 2u);

    // Read the payload the compositor writes into the receive() pipe.
    wl::FdHandle fd = app.data_device.Receive("text/plain");
    ASSERT_GE(fd.Get(), 0);
    std::string got;
    std::array<char, 64> buf{};
    ssize_t n = 0;
    while ((n = read(fd.Get(), buf.data(), buf.size())) > 0)
      got.append(buf.data(), static_cast<std::size_t>(n));
    EXPECT_EQ(got, kPayload);

    app.data_device.Release();
    if (seat != nullptr)
      wl_proxy_destroy(seat);
  }

  wl_display_disconnect(client);
  wl_display_terminate(server);
  server_thread.join();
  wl_display_destroy(server);
}

// ── Copy side: wl::DataDevice::Offer + wl_data_source
// ─────────────────────────
//
// The client takes the clipboard with a data source; the compositor then
// requests the data via wl_data_source.send, and the client's OnSend writes the
// payload into the pipe the compositor reads.

// The compositor stashes the read end of the receive pipe here for the test's
// main thread to drain (reading it on the server event thread would deadlock).
struct CopyCtx {
  std::atomic<int> read_fd{-1};
};

// wl_data_source requests: offer, set_actions, destroy.
void SourceOfferStub(wl_client* /*client*/,
                     wl_resource* /*source*/,
                     const char* /*mime*/) {}
struct SourceImpl {
  void (*offer)(wl_client*, wl_resource*, const char*);
  void (*set_actions)(wl_client*, wl_resource*, uint32_t);
  void (*destroy)(wl_client*, wl_resource*);
};
constexpr SourceImpl kSourceImpl{&SourceOfferStub, nullptr,
                                 &ServerDestroyResource};

void CopyCreateDataSource(wl_client* client, wl_resource* mgr, uint32_t id) {
  wl_resource* src =
      wl_resource_create(client, SourceIface(), wl_resource_get_version(mgr),
                         static_cast<int>(id));
  wl_resource_set_implementation(src, &kSourceImpl, nullptr, nullptr);
}

// set_selection(source, serial): ask the client's source to write "text/plain"
// into a pipe, and hand the read end to the test.
void CopySetSelection(wl_client* /*client*/,
                      wl_resource* device,
                      wl_resource* source,
                      uint32_t /*serial*/) {
  if (source == nullptr)
    return;
  auto* ctx = static_cast<CopyCtx*>(wl_resource_get_user_data(device));
  std::array<int, 2> p{-1, -1};
  if (pipe2(p.data(), O_CLOEXEC) != 0)
    return;
  wl_resource_post_event(source, wc::wl_data_source_traits::Evt::Send,
                         "text/plain", p[1]);
  close(p[1]);
  ctx->read_fd.store(p[0], std::memory_order_release);
}

struct CopyDeviceImpl {
  void (*start_drag)(wl_client*,
                     wl_resource*,
                     wl_resource*,
                     wl_resource*,
                     wl_resource*,
                     uint32_t);
  void (*set_selection)(wl_client*, wl_resource*, wl_resource*, uint32_t);
  void (*release)(wl_client*, wl_resource*);
};
constexpr CopyDeviceImpl kCopyDeviceImpl{nullptr, &CopySetSelection,
                                         &ServerDestroyResource};

void CopyGetDataDevice(wl_client* client,
                       wl_resource* mgr,
                       uint32_t id,
                       wl_resource* /*seat*/) {
  wl_resource* dev =
      wl_resource_create(client, DeviceIface(), wl_resource_get_version(mgr),
                         static_cast<int>(id));
  // Propagate the CopyCtx (manager user_data) to the device for set_selection.
  wl_resource_set_implementation(dev, &kCopyDeviceImpl,
                                 wl_resource_get_user_data(mgr), nullptr);
}

struct CopyManagerImpl {
  void (*create_data_source)(wl_client*, wl_resource*, uint32_t);
  void (*get_data_device)(wl_client*, wl_resource*, uint32_t, wl_resource*);
};
constexpr CopyManagerImpl kCopyManagerImpl{&CopyCreateDataSource,
                                           &CopyGetDataDevice};

void CopyManagerBind(wl_client* client,
                     void* data,
                     uint32_t version,
                     uint32_t id) {
  wl_resource* r = wl_resource_create(
      client, ManagerIface(), static_cast<int>(version), static_cast<int>(id));
  wl_resource_set_implementation(r, &kCopyManagerImpl, data, nullptr);
}

struct CopyApp {
  wl::DataDevice<CopyApp> data_device;
  std::string_view payload = kPayload;
  void OnSend(const char* /*mime*/, wl::FdHandle fd) const {
    const ssize_t n = write(fd.Get(), payload.data(), payload.size());
    (void)n;  // fd is closed when the handle is destroyed
  }
};

TEST(DataDeviceRoundtrip, OfferSourceServesRequestedData) {
  std::array<int, 2> sv{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  CopyCtx ctx;
  wl_display* server = wl_display_create();
  ASSERT_NE(server, nullptr);
  ASSERT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server, SeatIface(), 1, nullptr, &SeatBind);
  wl_global_create(server, ManagerIface(), 3, &ctx, &CopyManagerBind);
  std::thread server_thread([server] { wl_display_run(server); });

  wl_display* client = wl_display_connect_to_fd(sv[1]);
  ASSERT_NE(client, nullptr);

  {
    CopyApp app;
    wl_proxy* seat = nullptr;
    uint32_t seat_name = 0;
    uint32_t seat_ver = 0;

    wl::CRegistry registry;
    ASSERT_TRUE(registry.Create(client));
    registry.OnGlobal([&](wl::CRegistry&, uint32_t n, std::string_view iface,
                          uint32_t v) {
      if (iface == wc::wl_seat_traits::interface_name) {
        seat_name = n;
        seat_ver = v;
      } else if (iface == wc::wl_data_device_manager_traits::interface_name) {
        app.data_device.Record(n, v);
      }
    });
    ASSERT_NE(wl_display_roundtrip(client), -1);
    ASSERT_NE(seat_name, 0u);

    seat = registry.Bind<wc::wl_seat_traits>(
        seat_name, std::min(seat_ver, wc::wl_seat_traits::version));
    ASSERT_NE(seat, nullptr);
    ASSERT_TRUE(app.data_device.Bind(registry, &app));
    app.data_device.Start(client, seat);

    // Take the clipboard, offering one MIME type.
    wl::MimeSet mimes;
    mimes.Add("text/plain");
    app.data_device.Offer(mimes, 1u);
    // The roundtrip lets the server process set_selection, post send, and the
    // client dispatch it (OnSend writes the payload).
    ASSERT_NE(wl_display_roundtrip(client), -1);

    const int rfd = ctx.read_fd.load(std::memory_order_acquire);
    ASSERT_GE(rfd, 0);
    std::string got;
    std::array<char, 64> buf{};
    ssize_t n = 0;
    while ((n = read(rfd, buf.data(), buf.size())) > 0)
      got.append(buf.data(), static_cast<std::size_t>(n));
    close(rfd);
    EXPECT_EQ(got, kPayload);

    app.data_device.Release();
    if (seat != nullptr)
      wl_proxy_destroy(seat);
  }

  wl_display_disconnect(client);
  wl_display_terminate(server);
  server_thread.join();
  wl_display_destroy(server);
}

}  // namespace
