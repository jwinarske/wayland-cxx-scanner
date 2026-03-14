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
  const auto out = generate_client_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("#pragma once"));
}

TEST(CodegenClientCxx, ContainsNamespace) {
  const auto out = generate_client_cxx_header(make_proto());
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
  const auto out = generate_client_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("struct Op"));
  EXPECT_THAT(out, HasSubstr("Destroy = 0"));
  EXPECT_THAT(out, HasSubstr("Pong = 1"));
  EXPECT_THAT(out, HasSubstr("struct Evt"));
  EXPECT_THAT(out, HasSubstr("Ping = 0"));
}

TEST(CodegenClientCxx, ContainsSinceVersionDefaultsToOne) {
  const auto out = generate_client_cxx_header(make_proto());
  // Both Op and Evt carry a nested Since struct.
  EXPECT_THAT(out, HasSubstr("struct Since"));
  // All messages without a since attribute default to 1.
  EXPECT_THAT(out, HasSubstr("Destroy = 1"));
  EXPECT_THAT(out, HasSubstr("Ping = 1"));
}

TEST(CodegenClientCxx, SinceVersionReflectsXmlAttribute) {
  const auto proto = parse_protocol_from_string(R"(
<protocol name="agl">
  <interface name="agl_shell" version="3">
    <request name="open_window"/>
    <request name="set_ready" since="2"/>
    <event name="bound_ok"/>
    <event name="bound_fail" since="2"/>
  </interface>
</protocol>)");
  const auto out = generate_client_cxx_header(proto);
  EXPECT_THAT(out, HasSubstr("OpenWindow = 1"));   // since defaulted to 1
  EXPECT_THAT(out, HasSubstr("SetReady = 2"));     // since="2"
  EXPECT_THAT(out, HasSubstr("BoundOk = 1"));
  EXPECT_THAT(out, HasSubstr("BoundFail = 2"));
}

TEST(CodegenClientCxx, ContainsCRTPClass) {
  const auto out = generate_client_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("template <class Derived>"));
  EXPECT_THAT(out, HasSubstr("CXdgWmBase"));
  EXPECT_THAT(out, HasSubstr("wl::CProxyImpl"));
}

TEST(CodegenClientCxx, ContainsRequestMethod) {
  const auto out = generate_client_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("void Destroy("));
  EXPECT_THAT(out, HasSubstr("void Pong("));
}

TEST(CodegenClientCxx, ContainsEventHandlerAndMap) {
  const auto out = generate_client_cxx_header(make_proto());
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

TEST(CodegenClientCxx, DigitLeadingEnumEntryGetsPrefixed) {
  // Mirrors wl_output.transform which has entries "90", "180", "270".
  auto proto = parse_protocol_from_string(R"(
<protocol name="wayland">
  <interface name="wl_output" version="4">
    <enum name="transform">
      <entry name="normal" value="0"/>
      <entry name="90"     value="1"/>
      <entry name="180"    value="2"/>
      <entry name="270"    value="3"/>
    </enum>
  </interface>
</protocol>)");
  auto out = generate_client_cxx_header(proto);
  // Digits are spelled out so the generated enum-class value is valid C++.
  EXPECT_THAT(out, HasSubstr("Normal = 0"));
  EXPECT_THAT(out, HasSubstr("NineZero = 1"));
  EXPECT_THAT(out, HasSubstr("OneEightZero = 2"));
  EXPECT_THAT(out, HasSubstr("TwoSevenZero = 3"));
  EXPECT_THAT(out, Not(HasSubstr("\n    90 =")));
}

TEST(CodegenClientCxx, EmptyProtocol) {
  Protocol p;
  p.name = "empty";
  const auto out = generate_client_cxx_header(p);
  EXPECT_THAT(out, HasSubstr("#pragma once"));
  EXPECT_THAT(out, HasSubstr("namespace empty::client"));
}
