// SPDX-License-Identifier: MIT
#include "ir.hpp"

#include <gtest/gtest.h>

using namespace wl::scanner::ir;

TEST(Ir, ProtocolDefaultConstruct) {
    Protocol p;
    EXPECT_TRUE(p.name.empty());
    EXPECT_TRUE(p.interfaces.empty());
}

TEST(Ir, InterfaceDefaultVersion) {
    Interface iface;
    EXPECT_EQ(iface.version, 1u);
}

TEST(Ir, ArgTypeDefaultIsInt) {
    Arg arg;
    EXPECT_EQ(arg.type, ArgType::Int);
    EXPECT_FALSE(arg.nullable);
    EXPECT_FALSE(arg.allow_null);
}

TEST(Ir, EnumDefaultNotBitfield) {
    Enum en;
    EXPECT_FALSE(en.is_bitfield);
}

TEST(Ir, MessageDefaultOpcodeZero) {
    Message msg;
    EXPECT_EQ(msg.opcode, 0u);
    EXPECT_FALSE(msg.is_destructor);
}

TEST(Ir, ParseErrorIsException) {
    EXPECT_THROW(throw ParseError("test"), ParseError);
    EXPECT_THROW(throw ParseError("test"), std::runtime_error);
}

TEST(Ir, AllArgTypesDistinct) {
    // Ensure every ArgType value is distinct (no enum collision).
    EXPECT_NE(static_cast<int>(ArgType::Int), static_cast<int>(ArgType::Uint));
    EXPECT_NE(static_cast<int>(ArgType::Fixed), static_cast<int>(ArgType::String));
    EXPECT_NE(static_cast<int>(ArgType::Object), static_cast<int>(ArgType::NewId));
    EXPECT_NE(static_cast<int>(ArgType::Array), static_cast<int>(ArgType::Fd));
    EXPECT_NE(static_cast<int>(ArgType::Fd), static_cast<int>(ArgType::Enum));
}
