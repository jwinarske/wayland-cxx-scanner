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
  EXPECT_THAT(out, HasSubstr("version = 6"));
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
  EXPECT_THAT(out, HasSubstr("OpenWindow = 1"));  // since defaulted to 1
  EXPECT_THAT(out, HasSubstr("SetReady = 2"));    // since="2"
  EXPECT_THAT(out, HasSubstr("BoundOk = 1"));
  EXPECT_THAT(out, HasSubstr("BoundFail = 2"));
}

TEST(CodegenClientCxx, EmitsSinceVersionMacros) {
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
  // Preprocessor since-version macros (mirrors wayland-scanner): the interface
  // version plus one macro per request/event, value from the XML since
  // attribute (defaulting to 1). These let consumers gate version-dependent
  // handlers/requests with the preprocessor, which the constexpr Since
  // constants cannot.
  EXPECT_THAT(out, HasSubstr("#define AGL_SHELL_INTERFACE_VERSION 3"));
  EXPECT_THAT(out, HasSubstr("#define AGL_SHELL_OPEN_WINDOW_SINCE_VERSION 1"));
  EXPECT_THAT(out, HasSubstr("#define AGL_SHELL_SET_READY_SINCE_VERSION 2"));
  EXPECT_THAT(out, HasSubstr("#define AGL_SHELL_BOUND_OK_SINCE_VERSION 1"));
  EXPECT_THAT(out, HasSubstr("#define AGL_SHELL_BOUND_FAIL_SINCE_VERSION 2"));
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

TEST(CodegenClientCxx, ContainsDirectDispatchEventHandler) {
  const auto out = generate_client_cxx_header(make_proto());
  EXPECT_THAT(out, HasSubstr("virtual void OnPing("));
  // Direct CRTP dispatch — _EvtPing calls OnPing directly, no event map.
  EXPECT_THAT(out, HasSubstr("static void _EvtPing("));
  EXPECT_THAT(out, HasSubstr("->OnPing("));
  // No WTL message-map machinery.
  EXPECT_THAT(out, Not(HasSubstr("BEGIN_EVENT_MAP")));
  EXPECT_THAT(out, Not(HasSubstr("ProcessEvent")));
  EXPECT_THAT(out, Not(HasSubstr("_CrackEvent")));
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

// wl_registry.bind has a dynamic (interface-less) new_id: the target interface
// and version are chosen at runtime.  The scanner must emit a BindTraits-
// templated method that goes through the versioned dynamic-bind path
// (_MarshalBind / wl::bind), NOT the plain `Bind(name, id)` that dropped the
// interface-name string + version and produced broken wire output.
TEST(CodegenClientCxx, DynamicBindEmitsBindTraitsTemplate) {
  const auto out = generate_client_cxx_header(parse_protocol_from_string(R"(
<protocol name="wayland">
  <interface name="wl_registry" version="1">
    <request name="bind">
      <arg name="name" type="uint"/>
      <arg name="id" type="new_id"/>
    </request>
  </interface>
</protocol>)"));
  EXPECT_THAT(out, HasSubstr("template <typename BindTraits>"));
  EXPECT_THAT(out, HasSubstr("wl_proxy* Bind("));
  EXPECT_THAT(out, HasSubstr("_MarshalBind("));
  EXPECT_THAT(out, HasSubstr("&BindTraits::wl_iface()"));
  EXPECT_THAT(out, HasSubstr("wl_registry_traits::Op::Bind"));
  // The synthetic client-chosen version parameter replaces the raw new_id.
  EXPECT_THAT(out, HasSubstr("uint32_t version)"));
  // The broken plain-marshal form must be gone.
  EXPECT_THAT(out, Not(HasSubstr("void Bind(")));
}

// Regression guard: a *regular* new_id (one carrying an `interface`) must keep
// the normal request form and must NOT be mistaken for a dynamic bind.
TEST(CodegenClientCxx, RegularNewIdIsNotDynamicBind) {
  const auto out = generate_client_cxx_header(parse_protocol_from_string(R"(
<protocol name="wayland">
  <interface name="wl_compositor" version="6">
    <request name="create_surface">
      <arg name="id" type="new_id" interface="wl_surface"/>
    </request>
  </interface>
</protocol>)"));
  EXPECT_THAT(out, HasSubstr("void CreateSurface("));
  EXPECT_THAT(out, Not(HasSubstr("_MarshalBind(")));
  EXPECT_THAT(out, Not(HasSubstr("BindTraits")));
}
