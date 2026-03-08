// SPDX-License-Identifier: MIT
#include "codegen_c.hpp"

#include "ir.hpp"
#include "xml_parser.hpp"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace wl::scanner;
using namespace wl::scanner::ir;
using ::testing::HasSubstr;
using ::testing::Not;

static Protocol make_minimal() {
    return parse_protocol_from_string(R"(
<protocol name="test">
  <interface name="wl_foo" version="3">
    <enum name="error">
      <entry name="role" value="0"/>
      <entry name="defunct" value="1"/>
    </enum>
    <request name="destroy" type="destructor"/>
    <request name="do_thing">
      <arg name="val" type="uint"/>
    </request>
    <event name="done">
      <arg name="serial" type="uint"/>
    </event>
  </interface>
</protocol>)");
}

TEST(CodegenC, ContainsPragmaOnce) {
    auto out = generate_c_header(make_minimal());
    EXPECT_THAT(out, HasSubstr("#pragma once"));
}

TEST(CodegenC, ContainsExternC) {
    auto out = generate_c_header(make_minimal());
    EXPECT_THAT(out, HasSubstr("extern \"C\""));
}

TEST(CodegenC, ContainsInterfaceDeclaration) {
    auto out = generate_c_header(make_minimal());
    EXPECT_THAT(out, HasSubstr("wl_foo_interface"));
}

TEST(CodegenC, ContainsVersionMacro) {
    auto out = generate_c_header(make_minimal());
    EXPECT_THAT(out, HasSubstr("WL_FOO_INTERFACE_VERSION 3"));
}

TEST(CodegenC, ContainsRequestOpcode) {
    auto out = generate_c_header(make_minimal());
    EXPECT_THAT(out, HasSubstr("WL_FOO_DESTROY 0"));
    EXPECT_THAT(out, HasSubstr("WL_FOO_DO_THING 1"));
}

TEST(CodegenC, ContainsEventOpcode) {
    auto out = generate_c_header(make_minimal());
    EXPECT_THAT(out, HasSubstr("WL_FOO_DONE_EVENT 0"));
}

TEST(CodegenC, ContainsEnumValues) {
    auto out = generate_c_header(make_minimal());
    EXPECT_THAT(out, HasSubstr("WL_FOO_ERROR_ROLE = 0"));
    EXPECT_THAT(out, HasSubstr("WL_FOO_ERROR_DEFUNCT = 1"));
}

TEST(CodegenC, EmptyProtocolCompiles) {
    Protocol p;
    p.name = "empty";
    auto out = generate_c_header(p);
    EXPECT_THAT(out, HasSubstr("#pragma once"));
    EXPECT_THAT(out, Not(HasSubstr("interface_version")));
}
