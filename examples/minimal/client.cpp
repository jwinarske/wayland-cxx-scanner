// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Client side of the minimal roundtrip example.
//
// run_client(): connects to the Wayland socket given by WAYLAND_DISPLAY,
// binds wl_minimal, sends req_a(42), waits for evt_x(42), then disconnects.
// Returns EXIT_SUCCESS or EXIT_FAILURE.

// Include the generated client header first (pulls in wayland-client-core.h).
#include "minimal_client.hpp"

extern "C" {
#include <wayland-client.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

// ── wl_interface definition ───────────────────────────────────────────────────
//
// Each translation unit that references wl_iface() provides its own copy;
// the content is identical to the server-side copy in server.cpp.

static const wl_interface* wl_minimal_msg_types[] = {nullptr};

static const wl_message wl_minimal_request_descs[] = {
    {"req_a", "u", wl_minimal_msg_types},
    {"req_b", "", nullptr},
};
static const wl_message wl_minimal_event_descs[] = {
    {"evt_x", "u", wl_minimal_msg_types},
};

static const wl_interface wl_minimal_iface_def = {
    "wl_minimal",
    2,
    2,
    wl_minimal_request_descs,
    1,
    wl_minimal_event_descs,
};

namespace minimal::client {
const wl_interface& wl_minimal_traits::wl_iface() noexcept {
  return wl_minimal_iface_def;
}
}  // namespace minimal::client

// ── Client implementation ─────────────────────────────────────────────────────

// Concrete CRTP implementation: records the received evt_x serial.
class MinimalClient : public minimal::client::CWlMinimal<MinimalClient> {
 public:
  bool     done        = false;
  uint32_t echo_serial = 0;

  void OnEvtX(uint32_t serial) override {
    echo_serial = serial;
    done        = true;
  }
};

// ── Public entry point ────────────────────────────────────────────────────────

/// Connect to the compositor socket given by WAYLAND_DISPLAY, bind wl_minimal,
/// send req_a(42), wait for evt_x(42), then disconnect cleanly.
int run_client() {
  constexpr uint32_t kTestValue = 42;

  wl_display* display = wl_display_connect(nullptr);
  if (!display) {
    std::fprintf(stderr, "client: wl_display_connect failed: %s\n",
                 std::strerror(errno));
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(display);

  // Scan for the wl_minimal global.
  struct RegistryData {
    uint32_t name    = 0;
    uint32_t version = 0;
    bool     found   = false;
  } reg_data;

  const wl_registry_listener registry_listener = {
      .global =
          [](void* udata, wl_registry* /*reg*/, uint32_t name,
             const char* iface, uint32_t ver) {
            if (std::string_view(iface) == "wl_minimal") {
              auto* d    = static_cast<RegistryData*>(udata);
              d->name    = name;
              d->version = ver;
              d->found   = true;
            }
          },
      .global_remove = [](void* /*udata*/, wl_registry* /*reg*/,
                          uint32_t /*name*/) {},
  };

  wl_registry_add_listener(registry, &registry_listener, &reg_data);
  wl_display_roundtrip(display);  // collect all globals

  if (!reg_data.found) {
    std::fprintf(stderr, "client: wl_minimal not advertised\n");
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return EXIT_FAILURE;
  }

  // Bind to wl_minimal version 1.
  auto* proxy = static_cast<wl_proxy*>(wl_registry_bind(
      registry, reg_data.name,
      &minimal::client::wl_minimal_traits::wl_iface(), 1));
  if (!proxy) {
    std::fprintf(stderr, "client: wl_registry_bind failed\n");
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return EXIT_FAILURE;
  }

  MinimalClient client_obj;
  client_obj._SetProxy(proxy);

  // Send req_a(42).
  client_obj.ReqA(kTestValue);
  wl_display_flush(display);

  // Dispatch until we receive evt_x.
  while (!client_obj.done) {
    if (wl_display_dispatch(display) < 0) {
      std::fprintf(stderr, "client: wl_display_dispatch error: %s\n",
                   std::strerror(errno));
      wl_display_disconnect(display);
      return EXIT_FAILURE;
    }
  }

  if (client_obj.echo_serial != kTestValue) {
    std::fprintf(stderr, "client: expected serial %u, got %u\n", kTestValue,
                 client_obj.echo_serial);
    wl_display_disconnect(display);
    return EXIT_FAILURE;
  }

  std::printf("client: roundtrip OK (echo=%u)\n", client_obj.echo_serial);

  // Send req_b (destructor) to cleanly destroy the server-side resource,
  // then disconnect the display.
  client_obj.ReqB();
  wl_display_flush(display);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return EXIT_SUCCESS;
}
