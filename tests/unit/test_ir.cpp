// SPDX-License-Identifier: MIT
#include "ir.hpp"
#include "xml_parser.hpp"

#include <gtest/gtest.h>

using namespace wl::scanner::ir;

TEST(Ir, ProtocolDefaultConstruct) {
  const Protocol p;
  EXPECT_TRUE(p.name.empty());
  EXPECT_TRUE(p.interfaces.empty());
}

TEST(Ir, InterfaceDefaultVersion) {
  const Interface iface;
  EXPECT_EQ(iface.version, 1u);
}

TEST(Ir, ArgTypeDefaultIsInt) {
  const Arg arg;
  EXPECT_EQ(arg.type, ArgType::Int);
  EXPECT_FALSE(arg.nullable);
  EXPECT_FALSE(arg.allow_null);
}

TEST(Ir, EnumDefaultNotBitfield) {
  const Enum en;
  EXPECT_FALSE(en.is_bitfield);
}

TEST(Ir, MessageDefaultOpcodeZero) {
  const Message msg;
  EXPECT_EQ(msg.opcode, 0u);
  EXPECT_FALSE(msg.is_destructor);
  EXPECT_TRUE(msg.since.empty());
}

TEST(Ir, SinceFieldParsedFromXml) {
  using namespace wl::scanner;
  auto [name, interfaces] = parse_protocol_from_string(R"(
<protocol name="test">
  <interface name="wl_foo" version="3">
    <request name="old_req"/>
    <request name="new_req" since="3"/>
    <event name="old_evt"/>
    <event name="new_evt" since="2"/>
  </interface>
</protocol>)");
  ASSERT_EQ(interfaces.size(), 1u);
  const auto& iface = interfaces[0];
  EXPECT_TRUE(iface.requests[0].since.empty());
  EXPECT_EQ(iface.requests[1].since, "3");
  EXPECT_TRUE(iface.events[0].since.empty());
  EXPECT_EQ(iface.events[1].since, "2");
}

TEST(Ir, ParseErrorIsException) {
  EXPECT_THROW(throw ParseError("test"), ParseError);
  EXPECT_THROW(throw ParseError("test"), std::runtime_error);
}

TEST(Ir, AllArgTypesDistinct) {
  // Ensure every ArgType value is distinct (no enum collision).
  EXPECT_NE(static_cast<int>(ArgType::Int), static_cast<int>(ArgType::Uint));
  EXPECT_NE(static_cast<int>(ArgType::Fixed),
            static_cast<int>(ArgType::String));
  EXPECT_NE(static_cast<int>(ArgType::Object),
            static_cast<int>(ArgType::NewId));
  EXPECT_NE(static_cast<int>(ArgType::Array), static_cast<int>(ArgType::Fd));
  EXPECT_NE(static_cast<int>(ArgType::Fd), static_cast<int>(ArgType::Enum));
}
