// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Live wl_display dispatch test for the event-created-object (new_id-in-event)
// codegen path.  This is the only tier that exercises the real C listener ABI:
// a hand-rolled libwayland *server* (correct by construction — it uses the raw
// server API) advertises a factory, and on the client's create request it
// spawns a child object and delivers it through the `spawn` event's new_id
// argument.  The *client* side is the generated CThingy proxy under test.
//
// Before the direction-aware cpp_arg_type fix, the generated `_EvtSpawn` thunk
// declared its new_id slot as `uint32_t` — a 4-byte slot for the 8-byte pointer
// libwayland passes, an ABI mismatch / UB.  With the fix the slot is
// `wl_proxy*` and this test passes cleanly under ASan+UBSan (it exercises the
// real C listener dispatch, so a truncated-width load would fault here).
// Against the unfixed codegen the base virtual is `OnSpawn(uint32_t)`, so the
// pointer-typed override below fails to compile — the buggy output cannot
// satisfy a type-correct consumer.  Either way, this is the regression gate.

#include "thingy_client.hpp"  // generated: namespace thingy::client

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

namespace {

const wl_interface* FactoryIface() {
  return &thingy::client::thingy_factory_traits::wl_iface();
}
const wl_interface* ThingyIface() {
  return &thingy::client::thingy_traits::wl_iface();
}

// ── Server side: raw libwayland-server (deliberately not the generated server
// header, so this test isolates the *client* codegen) ────────────────────────

// thingy_factory.create(new_id id): create the requested thingy, then spawn a
// child and announce it through the thingy.spawn event's new_id argument.
void ServerCreate(wl_client* client, wl_resource* /*factory*/, uint32_t id) {
  wl_resource* thing =
      wl_resource_create(client, ThingyIface(), 1, static_cast<int>(id));
  wl_resource_set_implementation(thing, nullptr, nullptr, nullptr);

  // id 0 → server-allocated id; this is the object delivered via the event.
  wl_resource* child = wl_resource_create(client, ThingyIface(), 1, 0);
  wl_resource_set_implementation(child, nullptr, nullptr, nullptr);

  constexpr uint32_t kSpawnOpcode = 0;  // thingy.spawn is the first event
  wl_resource_post_event(thing, kSpawnOpcode, child);
}

// Request vtable for thingy_factory (one request: create).
struct FactoryImpl {
  void (*create)(wl_client*, wl_resource*, uint32_t);
};
constexpr FactoryImpl kFactoryImpl{&ServerCreate};

void ServerBind(wl_client* client,
                void* /*data*/,
                uint32_t version,
                uint32_t id) {
  wl_resource* r = wl_resource_create(
      client, FactoryIface(), static_cast<int>(version), static_cast<int>(id));
  wl_resource_set_implementation(r, &kFactoryImpl, nullptr, nullptr);
}

// ── Client side: the generated proxies under test ────────────────────────────

struct FactoryHandler : thingy::client::CThingyFactory<FactoryHandler> {};

struct ThingHandler : thingy::client::CThingy<ThingHandler> {
  wl_proxy* spawned = nullptr;
  bool got_spawn = false;
  void OnSpawn(wl_proxy* child) override {
    got_spawn = true;
    spawned = child;
  }
};

TEST(EventNewIdRoundtrip, ClientReceivesTypedNonNullChildProxy) {
  std::array<int, 2> sv{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  // Server owns sv[0]; run its event loop on a background thread.
  wl_display* server = wl_display_create();
  ASSERT_NE(server, nullptr);
  // sv[0] is an already-connected socketpair end, so register it as a client
  // directly (wl_display_add_socket_fd is for *listening* sockets).
  ASSERT_NE(wl_client_create(server, sv[0]), nullptr);
  wl_global_create(server, FactoryIface(), 1, nullptr, &ServerBind);
  std::thread server_thread([server] { wl_display_run(server); });

  // Client owns sv[1].
  wl_display* client = wl_display_connect_to_fd(sv[1]);
  ASSERT_NE(client, nullptr);

  // All display-owned proxies (registry, factory, thing) live in this inner
  // scope so they are destroyed BEFORE wl_display_disconnect — destroying a
  // proxy after its display is gone would fault.
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

    // thingy_factory has no events, so it is adopted without a listener (like
    // wl_compositor in the examples) rather than via SetupHandler.
    wl::WlPtr<FactoryHandler> factory;
    wl_proxy* fraw = registry.Bind<thingy::client::thingy_factory_traits>(
        name, std::min(ver, thingy::client::thingy_factory_traits::version));
    ASSERT_NE(fraw, nullptr);
    factory.Attach(fraw);

    // create → the server spawns a child and posts thingy.spawn(new_id).
    wl::WlPtr<ThingHandler> thing;
    wl_proxy* raw =
        wl::construct<thingy::client::thingy_traits,
                      thingy::client::thingy_factory_traits::Op::Create>(
            *factory.Get());
    ASSERT_TRUE(wl::SetupHandler(thing, raw));

    ASSERT_NE(wl_display_roundtrip(client), -1);

    // The load-bearing assertions: the event delivered a real, pointer-width,
    // non-null proxy — impossible if the thunk slot were uint32_t.
    EXPECT_TRUE(thing.Get()->got_spawn);
    EXPECT_NE(thing.Get()->spawned, nullptr);

    // The event-created child has no owner; free it before the display goes.
    if (thing.Get()->spawned != nullptr)
      wl_proxy_destroy(thing.Get()->spawned);
  }

  wl_display_disconnect(client);
  wl_display_terminate(server);
  server_thread.join();
  wl_display_destroy(server);
}

}  // namespace
