// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Live wl_display dispatch test for wl::DataDevice driven by a *second*
// protocol family — ext-data-control — proving the templatized helper is
// protocol-agnostic.  ext-data-control is structurally isomorphic to core
// wl_data_device but focus-free: set_selection takes no serial and the device
// teardown is a plain destroy request.  A protocol-traits bundle selects it.
//
// The client uses wl_seat from core wayland.xml (get_data_device needs a seat)
// and the ext family for the clipboard.  A hand-rolled compositor advertises
// the ext manager, introduces an offer with its MIME types, marks it the
// selection, and serves the receive pipe.  Run under ASan+UBSan.

#include "dc_client.hpp"      // core wayland.xml (wl_seat, registry) + tables
#include "dc_ext_client.hpp"  // ext-data-control-v1 + tables

#include <wl/data_device.hpp>
#include <wl/registry.hpp>

extern "C" {
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-server-core.h>
}

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace wc = wayland::client;
namespace ec = ext_data_control_v1::client;

namespace {

constexpr std::string_view kPayload = "control-hello";
constexpr std::string_view kPrimaryPayload = "primary-hello";

// Protocol-traits bundle selecting the ext-data-control family: no serial on
// set_selection, and the device is torn down by a plain destroy request (so
// WlPtr::Reset sends it — not an explicit version-gated release).
struct ExtDataControlProtocol {
  using ManagerTraits = ec::ext_data_control_manager_v1_traits;
  using DeviceTraits = ec::ext_data_control_device_v1_traits;
  using OfferTraits = ec::ext_data_control_offer_v1_traits;
  using SourceTraits = ec::ext_data_control_source_v1_traits;
  template <class D>
  using ManagerBase = ec::CExtDataControlManagerV1<D>;
  template <class D>
  using DeviceBase = ec::CExtDataControlDeviceV1<D>;
  template <class D>
  using OfferBase = ec::CExtDataControlOfferV1<D>;
  template <class D>
  using SourceBase = ec::CExtDataControlSourceV1<D>;
  // Offer() (the only user of set_selection_takes_serial) is never instantiated
  // by this paste-only test, so clang's -Wunused-const-variable would fire.
  [[maybe_unused]] static constexpr bool set_selection_takes_serial = false;
  [[maybe_unused]] static constexpr bool device_release_is_explicit = false;
  static constexpr bool has_primary = true;
};

const wl_interface* SeatIface() {
  return &wc::wl_seat_traits::wl_iface();
}
const wl_interface* ManagerIface() {
  return &ec::ext_data_control_manager_v1_traits::wl_iface();
}
const wl_interface* DeviceIface() {
  return &ec::ext_data_control_device_v1_traits::wl_iface();
}
const wl_interface* OfferIface() {
  return &ec::ext_data_control_offer_v1_traits::wl_iface();
}

// ── Server: raw libwayland as an ext-data-control compositor ─────────────────

// Each offer resource carries the payload it should serve (regular vs primary)
// as its user data, so a single receive handler serves both channels.
void ServerReceive(wl_client* /*client*/,
                   wl_resource* offer,
                   const char* /*mime*/,
                   int32_t fd) {
  const auto* payload =
      static_cast<const std::string_view*>(wl_resource_get_user_data(offer));
  const std::string_view data = payload != nullptr ? *payload : kPayload;
  const ssize_t n = write(fd, data.data(), data.size());
  (void)n;
  close(fd);
}
void ServerDestroyResource(wl_client* /*client*/, wl_resource* resource) {
  wl_resource_destroy(resource);
}

// ext_data_control_offer_v1 requests: receive, destroy.
struct OfferImpl {
  void (*receive)(wl_client*, wl_resource*, const char*, int32_t);
  void (*destroy)(wl_client*, wl_resource*);
};
constexpr OfferImpl kOfferImpl{&ServerReceive, &ServerDestroyResource};

// ext_data_control_device_v1 requests: set_selection, destroy,
// set_primary_selection.
struct DeviceImpl {
  void (*set_selection)(wl_client*, wl_resource*, wl_resource*);
  void (*destroy)(wl_client*, wl_resource*);
  void (*set_primary_selection)(wl_client*, wl_resource*, wl_resource*);
};
constexpr DeviceImpl kDeviceImpl{nullptr, &ServerDestroyResource, nullptr};

// Introduce one offer carrying @p payload and mark it the selection named by
// @p selection_evt (Selection or PrimarySelection).
void PostOffer(wl_client* client,
               wl_resource* device,
               int ver,
               const std::string_view* payload,
               uint32_t selection_evt) {
  wl_resource* offer = wl_resource_create(client, OfferIface(), ver, 0);
  wl_resource_set_implementation(
      offer, &kOfferImpl, const_cast<std::string_view*>(payload), nullptr);
  wl_resource_post_event(
      device, ec::ext_data_control_device_v1_traits::Evt::DataOffer, offer);
  wl_resource_post_event(
      offer, ec::ext_data_control_offer_v1_traits::Evt::Offer, "text/plain");
  wl_resource_post_event(device, selection_evt, offer);
}

void ServerGetDataDevice(wl_client* client,
                         wl_resource* mgr,
                         uint32_t id,
                         wl_resource* /*seat*/) {
  const int ver = wl_resource_get_version(mgr);
  wl_resource* device =
      wl_resource_create(client, DeviceIface(), ver, static_cast<int>(id));
  wl_resource_set_implementation(device, &kDeviceImpl, nullptr, nullptr);

  // Announce the regular selection, then the primary.  This ordering is the one
  // that would clobber a single offer slot (the primary data_offer arriving
  // after the regular selection was committed) — the client must keep the two
  // channels in separate slots.
  PostOffer(client, device, ver, &kPayload,
            ec::ext_data_control_device_v1_traits::Evt::Selection);
  PostOffer(client, device, ver, &kPrimaryPayload,
            ec::ext_data_control_device_v1_traits::Evt::PrimarySelection);
}

// ext_data_control_manager_v1 requests: create_data_source, get_data_device,
// destroy.
struct ManagerImpl {
  void (*create_data_source)(wl_client*, wl_resource*, uint32_t);
  void (*get_data_device)(wl_client*, wl_resource*, uint32_t, wl_resource*);
  void (*destroy)(wl_client*, wl_resource*);
};
constexpr ManagerImpl kManagerImpl{nullptr, &ServerGetDataDevice,
                                   &ServerDestroyResource};

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
  wl_resource_create(client, SeatIface(), static_cast<int>(version),
                     static_cast<int>(id));
}

// ── Client: DataDevice bound to the ext protocol ─────────────────────────────

struct App {
  wl::DataDevice<App, ExtDataControlProtocol> data_device;
  wl::MimeSet mimes;
  wl::MimeSet primary_mimes;
  bool got_selection = false;
  bool got_primary = false;
  void OnSelection(const wl::MimeSet& m) {
    got_selection = true;
    mimes = m;
  }
  void OnPrimarySelection(const wl::MimeSet& m) {
    got_primary = true;
    primary_mimes = m;
  }
};

std::string DrainFd(int fd) {
  std::string got;
  std::array<char, 64> buf{};
  ssize_t n = 0;
  while ((n = read(fd, buf.data(), buf.size())) > 0)
    got.append(buf.data(), static_cast<std::size_t>(n));
  return got;
}

TEST(DataControlRoundtrip, ExtProtocolSeparatesRegularAndPrimarySelections) {
  std::array<int, 2> sv{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  wl_display* server = wl_display_create();
  ASSERT_NE(server, nullptr);
  ASSERT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server, SeatIface(), 1, nullptr, &SeatBind);
  wl_global_create(server, ManagerIface(), 1, nullptr, &ManagerBind);
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
    registry.OnGlobal(
        [&](wl::CRegistry&, uint32_t n, std::string_view iface, uint32_t v) {
          if (iface == wc::wl_seat_traits::interface_name) {
            seat_name = n;
            seat_ver = v;
          } else if (iface ==
                     ec::ext_data_control_manager_v1_traits::interface_name) {
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

    EXPECT_TRUE(app.got_selection);
    EXPECT_TRUE(app.got_primary);
    EXPECT_TRUE(app.mimes.Contains("text/plain"));
    EXPECT_TRUE(app.primary_mimes.Contains("text/plain"));
    EXPECT_TRUE(app.data_device.HasSelection());
    EXPECT_TRUE(app.data_device.HasPrimarySelection());

    // Separation: the regular offer must still yield the regular payload even
    // though the primary selection was announced afterward (a single offer slot
    // would have let the primary offer overwrite it).
    wl::FdHandle fd = app.data_device.Receive("text/plain");
    ASSERT_GE(fd.Get(), 0);
    EXPECT_EQ(DrainFd(fd.Get()), kPayload);

    wl::FdHandle pfd = app.data_device.ReceivePrimary("text/plain");
    ASSERT_GE(pfd.Get(), 0);
    EXPECT_EQ(DrainFd(pfd.Get()), kPrimaryPayload);

    app.data_device.Release();
    if (seat != nullptr)
      wl_proxy_destroy(seat);
  }

  wl_display_disconnect(client);
  wl_display_terminate(server);
  server_thread.join();
  // Reap the client explicitly: terminate can stop the loop before it processes
  // the disconnect above, and wl_display_destroy does not destroy surviving
  // clients, so their resources would leak on whichever run loses that race.
  wl_display_destroy_clients(server);
  wl_display_destroy(server);
}

}  // namespace
