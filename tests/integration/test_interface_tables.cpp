// SPDX-License-Identifier: MIT
// Compile-and-run validation of scanner-emitted wl_interface tables.
//
// The build generates iface_test_client.hpp from tests/fixtures/
// minimal_interfaces.xml with --emit-interface-tables.  This test links it
// against libwayland and checks that the emitted wl_interface / wl_message
// tables are structurally correct and that object/new_id slots point at the
// right interface objects -- including pointer identity with the real
// libwayland wl_surface_interface for the external reference.
#include <wayland-client.h>  // real wl_surface_interface

#include "iface_test_client.hpp"

#include <gtest/gtest.h>
#include <cstring>

namespace c = iface_test::client;

TEST(InterfaceTables, ManagerInterfaceShape) {
  const wl_interface& mgr = c::wl_thing_manager_traits::wl_iface();
  EXPECT_STREQ(mgr.name, "wl_thing_manager");
  EXPECT_EQ(mgr.version, 2);
  EXPECT_EQ(mgr.method_count, 3);
  EXPECT_EQ(mgr.event_count, 0);
}

TEST(InterfaceTables, FactoryNewIdSlot) {
  const wl_interface& mgr = c::wl_thing_manager_traits::wl_iface();
  const wl_message& create = mgr.methods[0];
  EXPECT_STREQ(create.name, "create_thing");
  EXPECT_STREQ(create.signature, "n");
  // new_id slot -> wl_thing's own table.
  EXPECT_EQ(create.types[0], &c::wl_thing_traits::wl_iface());
}

TEST(InterfaceTables, ObjectAndExternalSlots) {
  const wl_interface& mgr = c::wl_thing_manager_traits::wl_iface();
  const wl_message& attach = mgr.methods[1];
  EXPECT_STREQ(attach.name, "attach_surface");
  EXPECT_STREQ(attach.signature, "o?o");
  EXPECT_EQ(attach.types[0], &c::wl_thing_traits::wl_iface());
  // External reference resolves to the genuine libwayland symbol.
  EXPECT_EQ(attach.types[1], &wl_surface_interface);
}

TEST(InterfaceTables, FdAndSinceAndEnumSignatures) {
  const wl_interface& thing = c::wl_thing_traits::wl_iface();
  EXPECT_STREQ(thing.name, "wl_thing");
  EXPECT_EQ(thing.method_count, 3);
  EXPECT_STREQ(thing.methods[0].signature, "hu");  // set_keymap: fd + uint
  EXPECT_STREQ(thing.methods[1].signature, "2u");  // set_caps: since=2 + enum
}

TEST(InterfaceTables, EventDeliveredNewId) {
  const wl_interface& thing = c::wl_thing_traits::wl_iface();
  EXPECT_EQ(thing.event_count, 2);
  const wl_message& spawn = thing.events[0];
  EXPECT_STREQ(spawn.name, "spawn");
  EXPECT_STREQ(spawn.signature, "n");
  EXPECT_EQ(spawn.types[0], &c::wl_thing_traits::wl_iface());
  EXPECT_STREQ(thing.events[1].signature,
               "a?s");  // state: array + nullable str
}
