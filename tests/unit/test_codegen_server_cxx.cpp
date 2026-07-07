// SPDX-License-Identifier: MIT
#include "codegen_server_cxx.hpp"
#include "ir.hpp"
#include "xml_parser.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace wl::scanner;
using namespace wl::scanner::ir;
using ::testing::HasSubstr;
using ::testing::Not;

static Protocol make_proto() {
  return parse_protocol_from_string(R"(
<protocol name="xdg_shell">
  <interface name="xdg_wm_base" version="6">
    <request name="destroy" type="destructor"/>
    <request name="pong">
      <arg name="serial" type="uint"/>
    </request>
    <event name="ping">
      <arg name="serial" type="uint"/>
    </event>
  </interface>
</protocol>)");
}

TEST(CodegenServerCxx, ContainsPragmaOnce) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("#pragma once"));
}

TEST(CodegenServerCxx, ContainsNamespace) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("namespace xdg_shell::server"));
}

TEST(CodegenServerCxx, ContainsServerTraitsStruct) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("xdg_wm_base_server_traits"));
  EXPECT_THAT(out, HasSubstr("\"xdg_wm_base\""));
  EXPECT_THAT(out, HasSubstr("version = 6"));
}

TEST(CodegenServerCxx, ContainsOpcodeConstants) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("struct Req"));
  EXPECT_THAT(out, HasSubstr("Destroy = 0"));
  EXPECT_THAT(out, HasSubstr("Pong = 1"));
  EXPECT_THAT(out, HasSubstr("struct Evt"));
  EXPECT_THAT(out, HasSubstr("Ping = 0"));
}

TEST(CodegenServerCxx, ContainsSinceVersionDefaultsToOne) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("struct Since"));
  EXPECT_THAT(out, HasSubstr("Destroy = 1"));
  EXPECT_THAT(out, HasSubstr("Ping = 1"));
}

TEST(CodegenServerCxx, SinceVersionReflectsXmlAttribute) {
  const auto proto = parse_protocol_from_string(R"(
<protocol name="agl">
  <interface name="agl_shell" version="3">
    <request name="open_window"/>
    <request name="set_ready" since="2"/>
    <event name="bound_fail" since="3"/>
  </interface>
</protocol>)");
  const auto out = generate_server_cxx_header(proto);
  EXPECT_THAT(out, HasSubstr("OpenWindow = 1"));
  EXPECT_THAT(out, HasSubstr("SetReady = 2"));
  EXPECT_THAT(out, HasSubstr("BoundFail = 3"));
}

TEST(CodegenServerCxx, ContainsCRTPServerClass) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("template <class Derived>"));
  EXPECT_THAT(out, HasSubstr("CXdgWmBaseServer"));
  EXPECT_THAT(out, HasSubstr("wl::CResourceImpl"));
}

TEST(CodegenServerCxx, ContainsSendEventMethod) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("void SendPing("));
}

TEST(CodegenServerCxx, ContainsDirectDispatchRequestHandlers) {
  const auto out = generate_server_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("virtual void OnDestroy("));
  EXPECT_THAT(out, HasSubstr("virtual void OnPong("));
  // Direct CRTP dispatch — _ReqPong calls OnPong directly.
  EXPECT_THAT(out, HasSubstr("static void _ReqPong("));
  EXPECT_THAT(out, HasSubstr("->OnPong("));
  // No WTL request-map machinery.
  EXPECT_THAT(out, Not(HasSubstr("BEGIN_REQUEST_MAP")));
  EXPECT_THAT(out, Not(HasSubstr("ProcessRequest")));
  EXPECT_THAT(out, Not(HasSubstr("_CrackRequest")));
}

TEST(CodegenServerCxx, EmptyProtocol) {
  Protocol p;
  p.name = "empty";
  const auto out = generate_server_cxx_header(p);
  EXPECT_THAT(out, HasSubstr("#pragma once"));
  EXPECT_THAT(out, HasSubstr("namespace empty::server"));
}

// The server mirror of the client event new_id fix: a new_id in an *event* the
// server sends is a resource the server created, and wl_resource_post_event
// marshals it as a pointer — so the Send* slot must be wl_resource*, not a
// uint32_t passed through varargs.
static Protocol make_new_id_event_proto() {
  return parse_protocol_from_string(R"(
<protocol name="minimal">
  <interface name="wl_thing_manager" version="1">
    <request name="create_thing">
      <arg name="id" type="new_id" interface="wl_thing"/>
    </request>
  </interface>
  <interface name="wl_thing" version="1">
    <event name="spawn">
      <arg name="child" type="new_id" interface="wl_thing"/>
    </event>
  </interface>
</protocol>)");
}

TEST(CodegenServerCxx, EventNewIdSendIsResourcePointer) {
  const auto out = generate_server_cxx_header(make_new_id_event_proto());
  EXPECT_THAT(out, HasSubstr("void SendSpawn(wl_resource* child)"));
  EXPECT_THAT(out, Not(HasSubstr("void SendSpawn(uint32_t")));
}

// Regression guard: a new_id in an incoming *request* is still the
// client-chosen id (a uint32_t the server binds via wl_resource_create) —
// unchanged by the event-side fix, in both the virtual handler and the dispatch
// thunk.
TEST(CodegenServerCxx, RequestNewIdStaysUint32) {
  const auto out = generate_server_cxx_header(make_new_id_event_proto());
  EXPECT_THAT(out, HasSubstr("OnCreateThing(wl_client* /*client*/, "
                             "wl_resource* /*resource*/, uint32_t /*id*/)"));
  EXPECT_THAT(out, HasSubstr("_ReqCreateThing(wl_client* client, "
                             "wl_resource* resource, uint32_t a0)"));
}
