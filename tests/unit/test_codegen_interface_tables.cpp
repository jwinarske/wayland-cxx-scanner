// SPDX-License-Identifier: MIT
// Tests for the --emit-interface-tables code path: inline wl_interface tables
// and wl_iface() definitions emitted into client/server headers.
#include "codegen_client_cxx.hpp"
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
<protocol name="iface_test">
  <interface name="wl_thing_manager" version="2">
    <request name="create_thing">
      <arg name="id" type="new_id" interface="wl_thing"/>
    </request>
    <request name="attach_surface">
      <arg name="thing" type="object" interface="wl_thing"/>
      <arg name="surface" type="object" interface="wl_surface" allow-null="true"/>
    </request>
    <request name="destroy" type="destructor"/>
  </interface>
  <interface name="wl_thing" version="2">
    <enum name="caps" bitfield="true">
      <entry name="a" value="1"/>
    </enum>
    <request name="set_keymap">
      <arg name="fd" type="fd"/>
      <arg name="size" type="uint"/>
    </request>
    <request name="set_caps" since="2">
      <arg name="caps" type="uint" enum="caps"/>
    </request>
    <event name="spawn">
      <arg name="child" type="new_id" interface="wl_thing"/>
    </event>
    <event name="state">
      <arg name="data" type="array"/>
      <arg name="name" type="string" allow-null="true"/>
    </event>
  </interface>
</protocol>)");
}

// ── Default: tables are NOT emitted ──────────────────────────────────────────

TEST(InterfaceTables, OmittedByDefault) {
  const auto out = generate_client_cxx_header(make_proto());
  EXPECT_THAT(out, Not(HasSubstr("namespace detail")));
  EXPECT_THAT(out, Not(HasSubstr("iface_types")));
  // The traits still only DECLARE wl_iface() (no inline definition).
  EXPECT_THAT(out, Not(HasSubstr("wl_iface() noexcept {")));
}

// ── Opt-in: tables and wl_iface() definitions are emitted ────────────────────

TEST(InterfaceTables, EmittedWhenRequested) {
  const auto out =
      generate_client_cxx_header(make_proto(), CppStd::Cpp17, true);
  EXPECT_THAT(out, HasSubstr("namespace detail"));
  EXPECT_THAT(out, HasSubstr("inline const wl_interface* iface_types[]"));
  EXPECT_THAT(out, HasSubstr("inline const wl_interface wl_thing_iface = {"));
  EXPECT_THAT(
      out, HasSubstr("inline const wl_interface wl_thing_manager_iface = {"));
  // wl_iface() is now defined inline, bound to the table.
  EXPECT_THAT(out, HasSubstr("wl_thing_traits::wl_iface() noexcept {\n"
                             "  return detail::wl_thing_iface;\n}"));
}

// External interfaces (wl_surface) are forward-declared with C linkage so the
// table can point at the libwayland-provided symbol.
TEST(InterfaceTables, ForwardDeclaresExternalInterfaces) {
  const auto out =
      generate_client_cxx_header(make_proto(), CppStd::Cpp17, true);
  EXPECT_THAT(out, HasSubstr("extern \"C\" {"));
  EXPECT_THAT(out,
              HasSubstr("extern const wl_interface wl_surface_interface;"));
  // Internal interfaces are not declared extern "C".
  EXPECT_THAT(out, HasSubstr("extern const wl_interface wl_thing_iface;"));
}

// ── Wire signatures ──────────────────────────────────────────────────────────

TEST(InterfaceTables, MessageSignatures) {
  const auto out =
      generate_client_cxx_header(make_proto(), CppStd::Cpp17, true);
  EXPECT_THAT(out, HasSubstr("{\"create_thing\", \"n\","));  // factory new_id
  EXPECT_THAT(
      out, HasSubstr("{\"attach_surface\", \"o?o\","));  // obj + nullable obj
  EXPECT_THAT(out, HasSubstr("{\"destroy\", \"\", nullptr}"));  // no args
  EXPECT_THAT(out, HasSubstr("{\"set_keymap\", \"hu\","));      // fd + uint
  EXPECT_THAT(out, HasSubstr("{\"set_caps\", \"2u\","));  // since=2 + enum
  EXPECT_THAT(out, HasSubstr("{\"spawn\", \"n\","));      // event new_id
  EXPECT_THAT(out, HasSubstr("{\"state\", \"a?s\","));  // array + nullable str
}

// ── Object/new_id slots point at the right interface table ───────────────────

TEST(InterfaceTables, ObjectSlotsReferenceInterfaces) {
  const auto out =
      generate_client_cxx_header(make_proto(), CppStd::Cpp17, true);
  // create_thing's new_id slot -> wl_thing; attach_surface's object slots ->
  // wl_thing (internal) and wl_surface (external).
  EXPECT_THAT(out, HasSubstr("&wl_thing_iface"));
  EXPECT_THAT(out, HasSubstr("&wl_surface_interface"));
}

// ── Server headers get the same treatment with the server traits suffix ──────

TEST(InterfaceTables, ServerBindsServerTraits) {
  const auto out =
      generate_server_cxx_header(make_proto(), CppStd::Cpp17, true);
  EXPECT_THAT(out, HasSubstr("namespace detail"));
  EXPECT_THAT(out, HasSubstr("wl_thing_server_traits::wl_iface() noexcept {\n"
                             "  return detail::wl_thing_iface;\n}"));
}
