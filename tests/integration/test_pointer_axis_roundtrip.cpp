// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Live wl_pointer scroll dispatch test.  A hand-rolled libwayland *server*
// advertises wl_seat at a chosen version and answers get_pointer with a real
// axis event sequence; the *client* is wl::PointerHandler driving the generated
// CWlPointer thunk.  Everything crosses a socketpair, so the generated listener
// is invoked through the real C ABI.
//
// The unit tests in unit/test_seat.cpp call the handler's virtuals directly,
// which proves the normalization arithmetic but says nothing about how an axis
// event gets from the wire into those virtuals.  Only this tier covers:
//
//   • opcode mapping — six axis-family events sit at opcodes 4 and 6-9, and a
//     misrouted one would silently deliver the wrong event;
//   • argument order and width, including the wl_fixed_t → double conversion;
//   • every version-dependent path, since the handler takes the version from
//     the proxy: only a real negotiation can choose one, so the value120-from-8
//     preference and the pre-5 per-event flush are testable nowhere else.
//
// Event opcodes below are taken from wayland.xml, deliberately NOT from the
// generated header: checking generated dispatch against generated constants
// would be circular.

#include "wayland_client.hpp"  // generated — must precede the wl/ headers

#include <wl/client_helpers.hpp>
#include <wl/proxy_impl.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>  // wl_iface() for wl_seat/wl_pointer, wl::PointerHandler
#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-core.h>
#include <wayland-server-core.h>
}

#include <sys/socket.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// <wl/seat.hpp> defines wl_iface() for every interface SeatManager binds; the
// pointer needs a surface to enter, so this test supplies those two itself.
namespace wayland::client {
const wl_interface& wl_compositor_traits::wl_iface() noexcept {
  return wl_compositor_interface;
}
const wl_interface& wl_surface_traits::wl_iface() noexcept {
  return wl_surface_interface;
}
}  // namespace wayland::client

namespace {

using wayland::client::wl_pointer_traits;
using wayland::client::wl_seat_traits;

// ── Protocol constants, from wayland.xml ─────────────────────────────────────
constexpr uint32_t kEvtEnter = 0;         // since 1
constexpr uint32_t kEvtLeave = 1;         // since 1
constexpr uint32_t kEvtAxis = 4;          // since 1
constexpr uint32_t kEvtFrame = 5;         // since 5
constexpr uint32_t kEvtAxisSource = 6;    // since 5
constexpr uint32_t kEvtAxisStop = 7;      // since 5
constexpr uint32_t kEvtAxisDiscrete = 8;  // since 5
constexpr uint32_t kEvtAxisValue120 = 9;  // since 8

constexpr uint32_t kAxisVertical = 0;
constexpr uint32_t kAxisHorizontal = 1;
constexpr uint32_t kSourceWheel = 0;
constexpr uint32_t kSourceFinger = 1;

wl_fixed_t Fixed(double v) {
  return static_cast<wl_fixed_t>(v * 256.0);
}

// The event sequence a scenario wants delivered.  Invoked on the server thread
// from inside get_pointer, so it is queued before the client's next roundtrip
// returns — no cross-thread race and no sleeping.  Reaches the request handler
// through the global's user-data pointer rather than a file-scope variable.
using Sender = std::function<void(wl_resource*)>;

// ── Server: raw libwayland-server, correct by construction ───────────────────
// Note this TU cannot include wayland-server-protocol.h — it would redefine the
// enums and interface externs that wayland-client-protocol.h (via seat.hpp)
// already provides.  Hence raw opcodes and the generated wl_iface() tables.

// wl_compositor / wl_surface exist here only so the pointer has a real surface
// to enter: wl_pointer.enter names one, and the whole point of testing it over
// a socketpair is that the surface crosses the wire as an object id and comes
// back out of the generated thunk as a proxy.
wl_resource* g_server_surface = nullptr;

void SurfaceDestroy(wl_client*, wl_resource* r) {
  wl_resource_destroy(r);
}
struct SurfaceImpl {
  void (*destroy)(wl_client*, wl_resource*);
  void (*attach)(wl_client*, wl_resource*, wl_resource*, int32_t, int32_t);
  void (*damage)(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t);
  void (*frame)(wl_client*, wl_resource*, uint32_t);
  void (*set_opaque_region)(wl_client*, wl_resource*, wl_resource*);
  void (*set_input_region)(wl_client*, wl_resource*, wl_resource*);
  void (*commit)(wl_client*, wl_resource*);
  void (*set_buffer_transform)(wl_client*, wl_resource*, int32_t);
  void (*set_buffer_scale)(wl_client*, wl_resource*, int32_t);
  void (*damage_buffer)(wl_client*,
                        wl_resource*,
                        int32_t,
                        int32_t,
                        int32_t,
                        int32_t);
  void (*offset)(wl_client*, wl_resource*, int32_t, int32_t);
};
constexpr SurfaceImpl kSurfaceImpl{&SurfaceDestroy, nullptr, nullptr, nullptr,
                                   nullptr,         nullptr, nullptr, nullptr,
                                   nullptr,         nullptr, nullptr};

void CompositorCreateSurface(wl_client* client,
                             wl_resource* compositor,
                             uint32_t id) {
  g_server_surface = wl_resource_create(
      client, &wayland::client::wl_surface_traits::wl_iface(),
      wl_resource_get_version(compositor), static_cast<int>(id));
  wl_resource_set_implementation(g_server_surface, &kSurfaceImpl, nullptr,
                                 nullptr);
}
struct CompositorImpl {
  void (*create_surface)(wl_client*, wl_resource*, uint32_t);
  void (*create_region)(wl_client*, wl_resource*, uint32_t);
};
constexpr CompositorImpl kCompositorImpl{&CompositorCreateSurface, nullptr};

void CompositorBind(wl_client* client, void*, uint32_t version, uint32_t id) {
  wl_resource* r = wl_resource_create(
      client, &wayland::client::wl_compositor_traits::wl_iface(),
      static_cast<int>(version), static_cast<int>(id));
  wl_resource_set_implementation(r, &kCompositorImpl, nullptr, nullptr);
}

void PointerSetCursor(wl_client*,
                      wl_resource*,
                      uint32_t,
                      wl_resource*,
                      int32_t,
                      int32_t) {}
void PointerRelease(wl_client*, wl_resource* r) {
  wl_resource_destroy(r);
}

struct PointerImpl {
  void (*set_cursor)(wl_client*,
                     wl_resource*,
                     uint32_t,
                     wl_resource*,
                     int32_t,
                     int32_t);
  void (*release)(wl_client*, wl_resource*);
};
constexpr PointerImpl kPointerImpl{&PointerSetCursor, &PointerRelease};

void SeatGetPointer(wl_client* client, wl_resource* seat, uint32_t id) {
  // The pointer inherits the seat's negotiated version, exactly as a real
  // compositor does — that is what the client will read back off the proxy.
  wl_resource* p =
      wl_resource_create(client, &wl_pointer_traits::wl_iface(),
                         wl_resource_get_version(seat), static_cast<int>(id));
  wl_resource_set_implementation(p, &kPointerImpl, nullptr, nullptr);
  auto* send = static_cast<Sender*>(wl_resource_get_user_data(seat));
  if (send != nullptr && *send)
    (*send)(p);
}
void SeatGetOther(wl_client*, wl_resource*, uint32_t) {}
void SeatRelease(wl_client*, wl_resource* r) {
  wl_resource_destroy(r);
}

struct SeatImpl {
  void (*get_pointer)(wl_client*, wl_resource*, uint32_t);
  void (*get_keyboard)(wl_client*, wl_resource*, uint32_t);
  void (*get_touch)(wl_client*, wl_resource*, uint32_t);
  void (*release)(wl_client*, wl_resource*);
};
constexpr SeatImpl kSeatImpl{&SeatGetPointer, &SeatGetOther, &SeatGetOther,
                             &SeatRelease};

void SeatBind(wl_client* client, void* data, uint32_t version, uint32_t id) {
  wl_resource* r =
      wl_resource_create(client, &wl_seat_traits::wl_iface(),
                         static_cast<int>(version), static_cast<int>(id));
  // `data` is the scenario's Sender, forwarded from wl_global_create.
  wl_resource_set_implementation(r, &kSeatImpl, data, nullptr);
}

// ── Client: the handler under test ───────────────────────────────────────────

struct SeatStub : wayland::client::CWlSeat<SeatStub> {
  void OnCapabilities(uint32_t) override {}
  void OnName(const char*) override {}
};

// wl_compositor has no events; wl_surface's are irrelevant here.
struct CompositorStub : wayland::client::CWlCompositor<CompositorStub> {};
struct SurfaceStub : wayland::client::CWlSurface<SurfaceStub> {};

struct ScrollApp {
  std::vector<wl::PointerAxisEvent> axes;
  int frames = 0;
  // enter/leave, kept whole so the surface each names can be checked against
  // the proxy this client created.
  std::vector<wl::PointerEvent> enters;
  std::vector<wl::PointerEvent> leaves;

  void OnPointerAxis(const wl::PointerAxisEvent& e) { axes.push_back(e); }
  void OnPointerFrame() { ++frames; }
  void OnPointerEnter(const wl::PointerEvent& e) { enters.push_back(e); }
  void OnPointerLeave(const wl::PointerEvent& e) { leaves.push_back(e); }
};

// Drive one scenario end to end at a negotiated wl_seat version.
// @p out_surface, when non-null, receives the wl_surface proxy this client
// created — the value an enter/leave event should name.
void RunScenario(uint32_t version,
                 Sender send,
                 ScrollApp& app,
                 wl_proxy** out_surface = nullptr) {
  std::array<int, 2> sv{-1, -1};
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()), 0);

  wl_display* server = wl_display_create();
  ASSERT_NE(server, nullptr);
  ASSERT_NE(wl_client_create(server, sv[0]), nullptr);
  // `send` outlives the server thread: it is joined before this frame returns.
  ASSERT_NE(wl_global_create(server, &wl_seat_traits::wl_iface(),
                             static_cast<int>(version), &send, &SeatBind),
            nullptr);
  ASSERT_NE(wl_global_create(server,
                             &wayland::client::wl_compositor_traits::wl_iface(),
                             6, nullptr, &CompositorBind),
            nullptr);
  std::thread server_thread([server] { wl_display_run(server); });

  wl_display* client = wl_display_connect_to_fd(sv[1]);
  ASSERT_NE(client, nullptr);

  // Proxies must die before the display they belong to.
  {
    wl::CRegistry registry;
    ASSERT_TRUE(registry.Create(client));
    uint32_t name = 0;
    uint32_t advertised = 0;
    uint32_t comp_name = 0;
    registry.OnGlobal(
        [&](wl::CRegistry&, uint32_t n, std::string_view iface, uint32_t v) {
          if (iface == wl_seat_traits::interface_name) {
            name = n;
            advertised = v;
          } else if (iface ==
                     wayland::client::wl_compositor_traits::interface_name) {
            comp_name = n;
          }
        });
    ASSERT_NE(wl_display_roundtrip(client), -1);
    ASSERT_NE(name, 0u) << "wl_seat global not advertised";
    ASSERT_EQ(advertised, version);

    wl::WlPtr<SeatStub> seat;
    ASSERT_TRUE(wl::BindHandler<wl_seat_traits>(registry, seat, name, version));

    // A real surface for the pointer to enter.  wl_compositor has no events, so
    // it is adopted without a listener.
    wl::WlPtr<SurfaceStub> surface;
    ASSERT_NE(comp_name, 0u) << "wl_compositor global not advertised";
    wl::WlPtr<CompositorStub> compositor;
    wl_proxy* craw =
        registry.Bind<wayland::client::wl_compositor_traits>(comp_name, 6);
    ASSERT_NE(craw, nullptr);
    compositor.Attach(craw);
    wl_proxy* sraw =
        wl::construct<wayland::client::wl_surface_traits,
                      wayland::client::wl_compositor_traits::Op::CreateSurface>(
            *compositor.Get());
    ASSERT_NE(sraw, nullptr);
    ASSERT_TRUE(wl::SetupHandler(surface, sraw));
    if (out_surface != nullptr)
      *out_surface = sraw;
    // The server needs the surface to exist before it answers get_pointer.
    ASSERT_NE(wl_display_roundtrip(client), -1);

    wl::WlPtr<wl::PointerHandler<ScrollApp>> ptr;
    wl_proxy* raw =
        wl::construct<wl_pointer_traits, wl_seat_traits::Op::GetPointer>(
            *seat.Get());
    ASSERT_NE(raw, nullptr);
    ASSERT_TRUE(wl::SetupHandler(ptr, raw));
    ptr.Get()->app_ = &app;
    // The handler reads the version straight off this proxy, which is the only
    // reason a version-dependent path can be tested honestly at all: a handler
    // without a proxy has to assume the newest.
    ASSERT_EQ(wl_proxy_get_version(raw), version);

    // First roundtrip carries get_pointer to the server and brings the axis
    // events it sends back; the second guarantees they have been dispatched.
    ASSERT_NE(wl_display_roundtrip(client), -1);
    ASSERT_NE(wl_display_roundtrip(client), -1);
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

// ── Tests ────────────────────────────────────────────────────────────────────

// ── enter/leave carry the surface ────────────────────────────────────────────
// The surface crosses the wire as an object id and has to come back out of the
// generated thunk as the very proxy this client created.  Nothing but a live
// dispatch can check that: a wrong-width or mis-declared object slot would hand
// the App a bogus pointer, and an App with several surfaces would then filter
// against garbage.
TEST(PointerAxisRoundtrip, EnterAndLeaveNameTheClientsSurface) {
  ScrollApp app;
  wl_proxy* client_surface = nullptr;
  RunScenario(
      8u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtEnter, 42u, g_server_surface, Fixed(12.5),
                               Fixed(34.25));
        wl_resource_post_event(p, kEvtLeave, 43u, g_server_surface);
      },
      app, &client_surface);

  ASSERT_NE(client_surface, nullptr);
  ASSERT_EQ(app.enters.size(), 1u);
  EXPECT_EQ(app.enters[0].surface, client_surface)
      << "enter must name the proxy this client created, not some other value";
  EXPECT_EQ(app.enters[0].serial, 42u);
  EXPECT_DOUBLE_EQ(app.enters[0].x, 12.5);
  EXPECT_DOUBLE_EQ(app.enters[0].y, 34.25);

  ASSERT_EQ(app.leaves.size(), 1u);
  EXPECT_EQ(app.leaves[0].surface, client_surface);
  EXPECT_EQ(app.leaves[0].serial, 43u);
}

TEST(PointerAxisRoundtrip, Value120WheelOnSeat8) {
  ScrollApp app;
  RunScenario(
      8u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtAxisSource, kSourceWheel);
        wl_resource_post_event(p, kEvtAxis, 4242u, kAxisVertical, Fixed(10.0));
        wl_resource_post_event(p, kEvtAxisValue120, kAxisVertical, 120);
        wl_resource_post_event(p, kEvtFrame);
      },
      app);

  ASSERT_EQ(app.axes.size(), 1u);
  // Each of these pins a different part of the wire→hook path: the opcode
  // routing, the argument order, and the wl_fixed_t decode.
  EXPECT_EQ(app.axes[0].axis, kAxisVertical);
  EXPECT_EQ(app.axes[0].value120, 120);
  EXPECT_DOUBLE_EQ(app.axes[0].continuous, 10.0);
  EXPECT_EQ(app.axes[0].source, kSourceWheel);
  EXPECT_EQ(app.axes[0].time, 4242u);
  EXPECT_FALSE(app.axes[0].stop);
  EXPECT_EQ(app.frames, 1);
}

TEST(PointerAxisRoundtrip, NegativeValue120SurvivesTheWire) {
  ScrollApp app;
  RunScenario(
      8u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtAxisSource, kSourceWheel);
        wl_resource_post_event(p, kEvtAxis, 1u, kAxisVertical, Fixed(-10.0));
        wl_resource_post_event(p, kEvtAxisValue120, kAxisVertical, -120);
        wl_resource_post_event(p, kEvtFrame);
      },
      app);

  ASSERT_EQ(app.axes.size(), 1u);
  EXPECT_EQ(app.axes[0].value120, -120) << "scroll up must stay negative";
  EXPECT_DOUBLE_EQ(app.axes[0].continuous, -10.0);
}

TEST(PointerAxisRoundtrip, DiscreteWheelOnSeat7) {
  ScrollApp app;
  RunScenario(
      7u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtAxisSource, kSourceWheel);
        wl_resource_post_event(p, kEvtAxis, 2u, kAxisVertical, Fixed(10.0));
        wl_resource_post_event(p, kEvtAxisDiscrete, kAxisVertical, 1);
        wl_resource_post_event(p, kEvtFrame);
      },
      app);

  ASSERT_EQ(app.axes.size(), 1u);
  EXPECT_EQ(app.axes[0].value120, 120) << "one detent on a seat without v120";
}

TEST(PointerAxisRoundtrip, FingerScrollSynthesizesNotchesOnSeat8) {
  ScrollApp app;
  RunScenario(
      8u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtAxisSource, kSourceFinger);
        wl_resource_post_event(p, kEvtAxis, 3u, kAxisVertical, Fixed(5.0));
        wl_resource_post_event(p, kEvtFrame);
      },
      app);

  ASSERT_EQ(app.axes.size(), 1u);
  EXPECT_EQ(app.axes[0].source, kSourceFinger);
  EXPECT_EQ(app.axes[0].value120, 60) << "half a notch";
  EXPECT_DOUBLE_EQ(app.axes[0].continuous, 5.0);
}

TEST(PointerAxisRoundtrip, AxisStopIsFlagged) {
  ScrollApp app;
  RunScenario(
      8u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtAxisSource, kSourceFinger);
        wl_resource_post_event(p, kEvtAxisStop, 9u, kAxisVertical);
        wl_resource_post_event(p, kEvtFrame);
      },
      app);

  ASSERT_EQ(app.axes.size(), 1u);
  EXPECT_TRUE(app.axes[0].stop);
  EXPECT_EQ(app.axes[0].value120, 0);
  EXPECT_EQ(app.axes[0].time, 9u);
}

TEST(PointerAxisRoundtrip, BothAxesInOneFrame) {
  ScrollApp app;
  RunScenario(
      8u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtAxisSource, kSourceWheel);
        wl_resource_post_event(p, kEvtAxis, 5u, kAxisVertical, Fixed(10.0));
        wl_resource_post_event(p, kEvtAxisValue120, kAxisVertical, 120);
        wl_resource_post_event(p, kEvtAxis, 5u, kAxisHorizontal, Fixed(-10.0));
        wl_resource_post_event(p, kEvtAxisValue120, kAxisHorizontal, -120);
        wl_resource_post_event(p, kEvtFrame);
      },
      app);

  ASSERT_EQ(app.axes.size(), 2u);
  EXPECT_EQ(app.axes[0].axis, kAxisVertical);
  EXPECT_EQ(app.axes[0].value120, 120);
  EXPECT_EQ(app.axes[1].axis, kAxisHorizontal);
  EXPECT_EQ(app.axes[1].value120, -120);
  EXPECT_EQ(app.frames, 1) << "one frame however many axes moved";
}

TEST(PointerAxisRoundtrip, PreV5SeatFlushesWithoutAFrameEvent) {
  ScrollApp app;
  // Seat 4 has no frame, no axis_source and no axis_discrete: the lone axis
  // event has to be delivered on its own or a v4 compositor scrolls nothing.
  RunScenario(
      4u,
      [](wl_resource* p) {
        wl_resource_post_event(p, kEvtAxis, 6u, kAxisVertical, Fixed(10.0));
      },
      app);

  ASSERT_EQ(app.axes.size(), 1u);
  EXPECT_EQ(app.axes[0].value120, 120);
  EXPECT_DOUBLE_EQ(app.axes[0].continuous, 10.0);
  EXPECT_EQ(app.axes[0].source, wl::kAxisSourceUnknown);
  EXPECT_EQ(app.frames, 1) << "each pre-v5 event is its own implicit frame";
}

}  // namespace
