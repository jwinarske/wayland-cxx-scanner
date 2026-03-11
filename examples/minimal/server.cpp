// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Server side of the minimal roundtrip example.
//
// Implements run_server(socket_name): creates a wl_display, advertises the
// wl_minimal global, handles req_a by echoing back evt_x, and terminates the
// event loop when the single client disconnects.

// Include the generated server header first (pulls in wayland-server-core.h).
#include "minimal_server.hpp"

extern "C" {
#include <wayland-server.h>
}

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// ── wl_interface definitions ─────────────────────────────────────────────────
//
// The generated traits declare wl_iface() but leave the definition to the
// consumer.  We provide the wl_interface data that libwayland uses for
// message dispatch and introspection.
//
// Argument-type signatures follow the Wayland wire protocol encoding:
//   u = uint32   i = int32   f = fixed   s = string   o = object
//   n = new_id   a = array   h = fd
//
// The `types` array has one slot per argument; slots for non-object arguments
// are NULL; slots for object/new_id arguments point to the target interface.

static const wl_interface* wl_minimal_msg_types[] = {nullptr};

static const wl_message wl_minimal_request_descs[] = {
    {"req_a", "u", wl_minimal_msg_types},  // one uint arg
    {"req_b", "", nullptr},                 // no args (destructor)
};
static const wl_message wl_minimal_event_descs[] = {
    {"evt_x", "u", wl_minimal_msg_types},  // one uint arg
};

// The interface object has internal linkage; both server and client
// translation units define their own copy with the same content.
static const wl_interface wl_minimal_iface_def = {
    "wl_minimal",
    2,
    2,
    wl_minimal_request_descs,
    1,
    wl_minimal_event_descs,
};

namespace minimal::server {
const wl_interface& wl_minimal_server_traits::wl_iface() noexcept {
  return wl_minimal_iface_def;
}
}  // namespace minimal::server

// ── Server implementation ─────────────────────────────────────────────────────

// Concrete CRTP implementation: echoes every req_a back as evt_x.
class MinimalServer
    : public minimal::server::CWlMinimalServer<MinimalServer> {
 public:
  void OnReqA(wl_client* /*client*/, wl_resource* /*resource*/,
               uint32_t value) override {
    SendEvtX(value);
  }

  // req_b is a destructor request: destroy the resource so the client's
  // wl_proxy_destroy() is acknowledged server-side.
  void OnReqB(wl_client* /*client*/, wl_resource* resource) override {
    wl_resource_destroy(resource);
  }
};

// ── Lifecycle helpers ─────────────────────────────────────────────────────────

static wl_display* s_display = nullptr;

// Destroy-listener for the wl_client: terminates the event loop once the
// single client disconnects so that wl_display_run() returns.
static wl_listener s_client_destroy_listener;

static void on_client_destroyed(wl_listener* /*lst*/, void* /*data*/) {
  wl_display_terminate(s_display);
}

// Called by libwayland when a client binds to our wl_minimal global.
static void on_bind(wl_client* client, void* /*data*/, uint32_t version,
                    uint32_t id) {
  auto* resource = wl_resource_create(
      client, &minimal::server::wl_minimal_server_traits::wl_iface(),
      static_cast<int>(version), id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  auto* impl = new MinimalServer();
  impl->_SetResource(resource);

  // Free the implementation when the resource is destroyed.
  wl_resource_set_destructor(resource, [](wl_resource* r) noexcept {
    delete static_cast<MinimalServer*>(wl_resource_get_user_data(r));
  });

  // Register a client destroy-listener so we can stop the event loop.
  s_client_destroy_listener.notify = on_client_destroyed;
  wl_client_add_destroy_listener(client, &s_client_destroy_listener);
}

// ── Public entry point ────────────────────────────────────────────────────────

/// Create a Wayland display, bind @p socket_name, then write the socket name
/// into @p ready_fd so the client knows the socket exists.  Runs the event
/// loop until the single client disconnects, then cleans up and returns
/// EXIT_SUCCESS or EXIT_FAILURE.
int run_server(const char* socket_name, int ready_fd) {
  s_display = wl_display_create();
  if (!s_display) {
    std::fprintf(stderr, "server: wl_display_create failed\n");
    return EXIT_FAILURE;
  }

  if (wl_display_add_socket(s_display, socket_name) != 0) {
    std::fprintf(stderr, "server: wl_display_add_socket(%s) failed: %s\n",
                 socket_name, std::strerror(errno));
    wl_display_destroy(s_display);
    return EXIT_FAILURE;
  }

  auto* global = wl_global_create(
      s_display, &minimal::server::wl_minimal_server_traits::wl_iface(),
      /*version=*/1, /*data=*/nullptr, on_bind);
  if (!global) {
    std::fprintf(stderr, "server: wl_global_create failed\n");
    wl_display_destroy(s_display);
    return EXIT_FAILURE;
  }

  // Signal the client: socket is live, write name into ready_fd.
  {
    auto len = static_cast<ssize_t>(std::strlen(socket_name));
    if (write(ready_fd, socket_name, static_cast<std::size_t>(len)) != len) {
      std::fprintf(stderr, "server: pipe write failed: %s\n",
                   std::strerror(errno));
      wl_global_destroy(global);
      wl_display_destroy(s_display);
      return EXIT_FAILURE;
    }
    close(ready_fd);
  }

  // Block until on_client_destroyed() calls wl_display_terminate().
  wl_display_run(s_display);

  wl_global_destroy(global);
  wl_display_destroy(s_display);
  return EXIT_SUCCESS;
}
