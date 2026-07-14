// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Live wl_display dispatch test for the SERVER event-created-object
// (new_id-in-event) codegen path — the server mirror of the client event test.
//
// The *server* side is the generated CThingyServer under test: its
// SendSpawn(wl_resource* child) marshals the child resource through
// wl_resource_post_event.  Before the direction-aware cpp_server_arg_type fix
// the parameter was `uint32_t`, so a resource pointer would be truncated /
// mismarshalled through the varargs of wl_resource_post_event.  A generated
// client receives the event and must see a valid, non-null child proxy.
//
// Only the server header carries --emit-interface-tables; the client's
// wl_iface() is bridged to those same tables below so both sides bind the same
// interface without defining it twice.

#include "srv_thingy_client.hpp"  // client-header, no interface tables
#include "srv_thingy_server.hpp"  // server-header, --emit-interface-tables

#include <wl/client_helpers.hpp>
#include <wl/proxy_impl.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-core.h>
#include <wayland-server-core.h>
}

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

// The client header declares wl_iface() but (without interface tables) leaves
// it undefined; a wl_interface is direction-agnostic, so bridge the client
// traits to the server's inline tables.
namespace thingy::client {
const wl_interface& thingy_factory_traits::wl_iface() noexcept {
  return thingy::server::thingy_factory_server_traits::wl_iface();
}
const wl_interface& thingy_traits::wl_iface() noexcept {
  return thingy::server::thingy_server_traits::wl_iface();
}
}  // namespace thingy::client

namespace {

const wl_interface* ThingyIface() {
  return &thingy::server::thingy_server_traits::wl_iface();
}

// ── Server side: the generated resource handlers under test ──────────────────

// thingy has no requests, so it has no request vtable and cannot use
// _SetResource; adopt the resource directly (protected member) — all we need is
// a bound resource to send the event on.
struct ThingyServer : thingy::server::CThingyServer<ThingyServer> {
  void Adopt(wl_resource* r) noexcept { m_resource = r; }
};

struct FactoryServer : thingy::server::CThingyFactoryServer<FactoryServer> {
  void OnCreate(wl_client* client,
                wl_resource* /*factory*/,
                uint32_t id) override {
    // Create the thingy the client asked for, then a server-allocated child,
    // and announce the child through the generated SendSpawn(wl_resource*).
    wl_resource* thing_res =
        wl_resource_create(client, ThingyIface(), 1, static_cast<int>(id));
    wl_resource* child_res = wl_resource_create(client, ThingyIface(), 1, 0);
    ThingyServer thing;
    thing.Adopt(thing_res);
    thing.SendSpawn(child_res);
  }
};

void ServerBind(wl_client* client,
                void* /*data*/,
                uint32_t version,
                uint32_t id) {
  wl_resource* r = wl_resource_create(
      client, &thingy::server::thingy_factory_server_traits::wl_iface(),
      static_cast<int>(version), static_cast<int>(id));
  auto* impl = new FactoryServer();
  impl->_SetResource(r);
  wl_resource_set_destructor(r, [](wl_resource* res) noexcept {
    delete static_cast<FactoryServer*>(wl_resource_get_user_data(res));
  });
}

// ── Client side: generated proxies receiving the event ───────────────────────

struct FactoryHandler : thingy::client::CThingyFactory<FactoryHandler> {};

struct ThingHandler : thingy::client::CThingy<ThingHandler> {
  wl_proxy* spawned = nullptr;
  bool got_spawn = false;
  void OnSpawn(wl_proxy* child) override {
    got_spawn = true;
    spawned = child;
  }
};

TEST(ServerEventNewIdRoundtrip, GeneratedServerSendsTypedChildResource) {
  std::array<int, 2> sv{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  wl_display* server = wl_display_create();
  ASSERT_NE(server, nullptr);
  ASSERT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server,
                   &thingy::server::thingy_factory_server_traits::wl_iface(), 1,
                   nullptr, &ServerBind);
  std::thread server_thread([server] { wl_display_run(server); });

  wl_display* client = wl_display_connect_to_fd(sv[1]);
  ASSERT_NE(client, nullptr);

  {
    wl::CRegistry registry;
    ASSERT_TRUE(registry.Create(client));
    uint32_t name = 0;
    uint32_t ver = 0;
    registry.OnGlobal(
        [&](wl::CRegistry&, uint32_t n, std::string_view iface, uint32_t v) {
          if (iface == thingy::client::thingy_factory_traits::interface_name) {
            name = n;
            ver = v;
          }
        });
    ASSERT_NE(wl_display_roundtrip(client), -1);
    ASSERT_NE(name, 0u) << "factory global not advertised";

    wl::WlPtr<FactoryHandler> factory;
    wl_proxy* fraw = registry.Bind<thingy::client::thingy_factory_traits>(
        name, std::min(ver, thingy::client::thingy_factory_traits::version));
    ASSERT_NE(fraw, nullptr);
    factory.Attach(fraw);

    wl::WlPtr<ThingHandler> thing;
    wl_proxy* raw =
        wl::construct<thingy::client::thingy_traits,
                      thingy::client::thingy_factory_traits::Op::Create>(
            *factory.Get());
    ASSERT_TRUE(wl::SetupHandler(thing, raw));

    ASSERT_NE(wl_display_roundtrip(client), -1);

    EXPECT_TRUE(thing.Get()->got_spawn);
    EXPECT_NE(thing.Get()->spawned, nullptr);

    if (thing.Get()->spawned != nullptr)
      wl_proxy_destroy(thing.Get()->spawned);
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
