// SPDX-License-Identifier: MIT
#include "codegen_client_cxx.hpp"
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
    <enum name="error">
      <entry name="role" value="0"/>
    </enum>
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

TEST(CodegenClientCxx, ContainsPragmaOnce) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("#pragma once"));
}

TEST(CodegenClientCxx, ContainsNamespace) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("namespace xdg_shell::client"));
}

TEST(CodegenClientCxx, ContainsTraitsStruct) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("xdg_wm_base_traits"));
    EXPECT_THAT(out, HasSubstr("interface_name"));
    EXPECT_THAT(out, HasSubstr("\"xdg_wm_base\""));
    EXPECT_THAT(out, HasSubstr("version        = 6"));
}

TEST(CodegenClientCxx, ContainsOpcodeConstants) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("struct Op"));
    EXPECT_THAT(out, HasSubstr("Destroy = 0"));
    EXPECT_THAT(out, HasSubstr("Pong = 1"));
    EXPECT_THAT(out, HasSubstr("struct Evt"));
    EXPECT_THAT(out, HasSubstr("Ping = 0"));
}

TEST(CodegenClientCxx, ContainsCRTPClass) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("template <class Derived>"));
    EXPECT_THAT(out, HasSubstr("CXdgWmBase"));
    EXPECT_THAT(out, HasSubstr("wl::CProxyImpl"));
}

TEST(CodegenClientCxx, ContainsRequestMethod) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("void Destroy("));
    EXPECT_THAT(out, HasSubstr("void Pong("));
}

TEST(CodegenClientCxx, ContainsEventHandlerAndMap) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("virtual void OnPing("));
    EXPECT_THAT(out, HasSubstr("BEGIN_EVENT_MAP(CXdgWmBase)"));
    EXPECT_THAT(out, HasSubstr("EVENT_HANDLER("));
    EXPECT_THAT(out, HasSubstr("END_EVENT_MAP()"));
}

TEST(CodegenClientCxx, ContainsEnumClass) {
    auto out = generate_client_cxx_header(make_proto());
    EXPECT_THAT(out, HasSubstr("enum class"));
    EXPECT_THAT(out, HasSubstr("Role = 0"));
}

TEST(CodegenClientCxx, EmptyProtocol) {
    Protocol p;
    p.name   = "empty";
    auto out = generate_client_cxx_header(p);
    EXPECT_THAT(out, HasSubstr("#pragma once"));
    EXPECT_THAT(out, HasSubstr("namespace empty::client"));
}
