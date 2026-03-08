// SPDX-License-Identifier: MIT
#include "codegen_server_cxx.hpp"

#include "ir.hpp"
#include "xml_parser.hpp"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

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
    auto out = generate_server_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("#pragma once"));
}

TEST(CodegenServerCxx, ContainsNamespace) {
    auto out = generate_server_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("namespace xdg_shell::server"));
}

TEST(CodegenServerCxx, ContainsServerTraitsStruct) {
    auto out = generate_server_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("xdg_wm_base_server_traits"));
    EXPECT_THAT(out, HasSubstr("\"xdg_wm_base\""));
    EXPECT_THAT(out, HasSubstr("version        = 6"));
}

TEST(CodegenServerCxx, ContainsOpcodeConstants) {
    auto out = generate_server_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("struct Req"));
    EXPECT_THAT(out, HasSubstr("Destroy = 0"));
    EXPECT_THAT(out, HasSubstr("Pong = 1"));
    EXPECT_THAT(out, HasSubstr("struct Evt"));
    EXPECT_THAT(out, HasSubstr("Ping = 0"));
}

TEST(CodegenServerCxx, ContainsCRTPServerClass) {
    auto out = generate_server_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("template <class Derived>"));
    EXPECT_THAT(out, HasSubstr("CXdgWmBaseServer"));
    EXPECT_THAT(out, HasSubstr("wl::CResourceImpl"));
}

TEST(CodegenServerCxx, ContainsSendEventMethod) {
    auto out = generate_server_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("void SendPing("));
}

TEST(CodegenServerCxx, ContainsRequestHandlersAndMap) {
    auto out = generate_server_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("virtual void OnDestroy("));
    EXPECT_THAT(out, HasSubstr("virtual void OnPong("));
    EXPECT_THAT(out, HasSubstr("BEGIN_REQUEST_MAP(CXdgWmBaseServer)"));
    EXPECT_THAT(out, HasSubstr("REQUEST_HANDLER("));
    EXPECT_THAT(out, HasSubstr("END_REQUEST_MAP()"));
}

TEST(CodegenServerCxx, EmptyProtocol) {
    Protocol p;
    p.name = "empty";
    auto out = generate_server_cxx_header(p);
    EXPECT_THAT(out, HasSubstr("#pragma once"));
    EXPECT_THAT(out, HasSubstr("namespace empty::server"));
}
