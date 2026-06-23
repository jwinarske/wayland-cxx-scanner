// SPDX-License-Identifier: MIT
// Regression test: a protocol whose argument names collide with the fixed
// dispatch-callback parameters (client/resource/self for requests, data for
// events) must still produce compilable generated code.  The build generates
// collide_test_client.hpp / collide_test_server.hpp from
// tests/fixtures/minimal_arg_collision.xml; if the forwarded arguments were not
// emitted positionally, the static dispatchers below would fail to compile.
#include "collide_test_client.hpp"
#include "collide_test_server.hpp"

#include <gtest/gtest.h>

struct CollideClient : collide_test::client::CWlCollide<CollideClient> {};
struct CollideServer : collide_test::server::CWlCollideServer<CollideServer> {};

// Explicit instantiation forces the static dispatch callbacks (where a
// colliding argument name would break compilation) to be instantiated.
template class collide_test::client::CWlCollide<CollideClient>;
template class collide_test::server::CWlCollideServer<CollideServer>;

TEST(DispatchArgCollision, GeneratedCodeCompiles) {
  CollideClient client;
  CollideServer server;
  (void)client;
  (void)server;
  SUCCEED();
}
