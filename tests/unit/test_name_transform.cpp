// SPDX-License-Identifier: MIT
#include "name_transform.hpp"

#include <gtest/gtest.h>

using namespace wl::scanner;

TEST(NameTransform, SnakeToPascalBasic) {
  EXPECT_EQ(snake_to_pascal("xdg_wm_base"), "XdgWmBase");
}
TEST(NameTransform, SnakeToPascalSingleWord) {
  EXPECT_EQ(snake_to_pascal("ping"), "Ping");
}
TEST(NameTransform, SnakeToPascalEmpty) {
  EXPECT_EQ(snake_to_pascal(""), "");
}
TEST(NameTransform, SnakeToPascalAlreadyCamel) {
  EXPECT_EQ(snake_to_pascal("wl_surface"), "WlSurface");
}

TEST(NameTransform, SnakeToCamelBasic) {
  EXPECT_EQ(snake_to_camel("xdg_wm_base"), "xdgWmBase");
}
TEST(NameTransform, SnakeToCamelSingleWord) {
  EXPECT_EQ(snake_to_camel("ping"), "ping");
}
TEST(NameTransform, SnakeToCamelEmpty) {
  EXPECT_EQ(snake_to_camel(""), "");
}

TEST(NameTransform, StripPrefixPascalRemovesPrefix) {
  // "xdg_wm_base_ping" with prefix "xdg_wm_base" → "Ping"
  EXPECT_EQ(strip_prefix_pascal("xdg_wm_base_ping", "xdg_wm_base"), "Ping");
}
TEST(NameTransform, StripPrefixPascalNoMatch) {
  EXPECT_EQ(strip_prefix_pascal("wl_surface", "xdg"), "WlSurface");
}
TEST(NameTransform, StripPrefixPascalExactPrefix) {
  // prefix == name (no trailing _) → fall back to pascal of whole name
  EXPECT_EQ(strip_prefix_pascal("wl_foo", "wl_foo"), "WlFoo");
}

TEST(NameTransform, EnumEntryToPascal) {
  EXPECT_EQ(enum_entry_to_pascal("error_role", "error"), "Role");
  EXPECT_EQ(enum_entry_to_pascal("role", "error"), "Role");
}

TEST(NameTransform, EnumEntryToPascalDigitLeading) {
  // Entries like "90", "180", "270" in wl_output.transform must have their
  // digits spelled out so the generated C++ enum-class value is valid.
  EXPECT_EQ(enum_entry_to_pascal("90",  "transform"), "NineZero");
  EXPECT_EQ(enum_entry_to_pascal("180", "transform"), "OneEightZero");
  EXPECT_EQ(enum_entry_to_pascal("270", "transform"), "TwoSevenZero");
  // Normal names must be unaffected.
  EXPECT_EQ(enum_entry_to_pascal("normal",  "transform"), "Normal");
  EXPECT_EQ(enum_entry_to_pascal("flipped", "transform"), "Flipped");
}

