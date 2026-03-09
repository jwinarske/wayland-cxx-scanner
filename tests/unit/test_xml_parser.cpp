// SPDX-License-Identifier: MIT
#include "ir.hpp"
#include "xml_parser.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <stdexcept>
#include <system_error>

using namespace wl::scanner;
using namespace wl::scanner::ir;

TEST(Parser, MinimalProtocol) {
  auto p = parse_protocol_from_string(
      R"(<protocol name="t"><interface name="wl_i" version="2"/></protocol>)");
  EXPECT_EQ(p.name, "t");
  ASSERT_EQ(p.interfaces.size(), 1u);
  EXPECT_EQ(p.interfaces[0].name, "wl_i");
  EXPECT_EQ(p.interfaces[0].version, 2u);
}

TEST(Parser, OpcodeNumberingIndependent) {
  auto p = parse_protocol_from_string(R"(
<protocol name="t">
  <interface name="wl_t" version="1">
    <request name="a"/><request name="b"/><request name="c"/>
    <event   name="x"/><event   name="y"/>
  </interface>
</protocol>)");
  auto& i = p.interfaces[0];
  EXPECT_EQ(i.requests[0].opcode, 0u);
  EXPECT_EQ(i.requests[1].opcode, 1u);
  EXPECT_EQ(i.requests[2].opcode, 2u);
  EXPECT_EQ(i.events[0].opcode, 0u);
  EXPECT_EQ(i.events[1].opcode, 1u);
}

TEST(Parser, AllArgTypesRecognised) {
  auto p = parse_protocol_from_string(R"(
<protocol name="t">
  <interface name="wl_t" version="1">
    <enum name="e"><entry name="v" value="0"/></enum>
    <request name="r">
      <arg name="a0" type="int"/>
      <arg name="a1" type="uint"/>
      <arg name="a2" type="fixed"/>
      <arg name="a3" type="string"/>
      <arg name="a4" type="object"/>
      <arg name="a5" type="new_id"/>
      <arg name="a6" type="array"/>
      <arg name="a7" type="fd"/>
      <arg name="a8" type="uint" enum="wl_t.e"/>
    </request>
  </interface>
</protocol>)");
  auto& args = p.interfaces[0].requests[0].args;
  ASSERT_EQ(args.size(), 9u);
  EXPECT_EQ(args[0].type, ArgType::Int);
  EXPECT_EQ(args[1].type, ArgType::Uint);
  EXPECT_EQ(args[2].type, ArgType::Fixed);
  EXPECT_EQ(args[3].type, ArgType::String);
  EXPECT_EQ(args[4].type, ArgType::Object);
  EXPECT_EQ(args[5].type, ArgType::NewId);
  EXPECT_EQ(args[6].type, ArgType::Array);
  EXPECT_EQ(args[7].type, ArgType::Fd);
  EXPECT_EQ(args[8].type, ArgType::Enum);
  EXPECT_EQ(args[8].enum_name, "wl_t.e");
}

TEST(Parser, InvalidXmlThrows) {
  EXPECT_THROW((void)parse_protocol_from_string("not xml"), ParseError);
}

TEST(Parser, UnknownArgTypeThrows) {
  EXPECT_THROW((void)parse_protocol_from_string(R"(
<protocol name="t">
  <interface name="i" version="1">
    <request name="r"><arg name="a" type="badtype"/></request>
  </interface>
</protocol>)"),
               ParseError);
}

TEST(Parser, MissingFileThrows) {
  EXPECT_THROW((void)parse_protocol("/no/such/file.xml"), std::system_error);
}

TEST(Parser, DestructorFlagParsed) {
  auto p = parse_protocol_from_string(R"(
<protocol name="t">
  <interface name="wl_t" version="1">
    <request name="destroy" type="destructor"/>
  </interface>
</protocol>)");
  EXPECT_TRUE(p.interfaces[0].requests[0].is_destructor);
}

TEST(Parser, BitfieldEnumParsed) {
  auto p = parse_protocol_from_string(R"(
<protocol name="t">
  <interface name="wl_t" version="1">
    <enum name="flags" bitfield="true">
      <entry name="read"  value="1"/>
      <entry name="write" value="2"/>
    </enum>
  </interface>
</protocol>)");
  auto& en = p.interfaces[0].enums[0];
  EXPECT_TRUE(en.is_bitfield);
  ASSERT_EQ(en.entries.size(), 2u);
  EXPECT_EQ(en.entries[0].name, "read");
  EXPECT_EQ(en.entries[0].value, 1u);
}

TEST(Parser, AllowNullAttribute) {
  auto p = parse_protocol_from_string(R"(
<protocol name="t">
  <interface name="wl_t" version="1">
    <request name="r">
      <arg name="s" type="string" allow-null="true"/>
    </request>
  </interface>
</protocol>)");
  EXPECT_TRUE(p.interfaces[0].requests[0].args[0].allow_null);
}

TEST(Parser, ParseFixtureFile) {
  // Tests that the bundled fixture can be loaded successfully.
  const char* srcdir = std::getenv("SRCDIR");
  if (!srcdir)
    GTEST_SKIP() << "SRCDIR not set";
  std::filesystem::path fixture =
      std::filesystem::path(srcdir) / "tests" / "fixtures" / "minimal.xml";
  auto p = parse_protocol(fixture);
  EXPECT_EQ(p.name, "minimal");
  EXPECT_FALSE(p.interfaces.empty());
}
