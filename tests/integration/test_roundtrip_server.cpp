// SPDX-License-Identifier: MIT
// Integration test: parse a protocol XML then generate a server header and
// verify the generated code structure against expected patterns.
#include "codegen_server_cxx.hpp"
#include "xml_parser.hpp"

#include <cstdlib>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>

using namespace wl::scanner;
using ::testing::HasSubstr;

static std::filesystem::path fixture_path() {
    const char* srcdir = std::getenv("SRCDIR");
    return srcdir ? std::filesystem::path(srcdir) / "tests" / "fixtures" / "minimal.xml"
                  : std::filesystem::path("tests/fixtures/minimal.xml");
}

TEST(RoundtripServer, ParseFixtureAndGenerateHeader) {
    std::error_code ec;
    if (!std::filesystem::exists(fixture_path(), ec))
        GTEST_SKIP() << "fixture not found: " << fixture_path();

    auto proto = parse_protocol(fixture_path());
    auto out   = generate_server_cxx_header(proto);

    EXPECT_THAT(out, HasSubstr("#pragma once"));
    EXPECT_THAT(out, HasSubstr("namespace minimal::server"));
    EXPECT_THAT(out, HasSubstr("wl_minimal_server_traits"));
    EXPECT_THAT(out, HasSubstr("CWlMinimalServer"));
    EXPECT_THAT(out, HasSubstr("wl::CResourceImpl"));
    EXPECT_THAT(out, HasSubstr("virtual void OnReqA("));
    EXPECT_THAT(out, HasSubstr("void SendEvtX("));
    EXPECT_THAT(out, HasSubstr("BEGIN_REQUEST_MAP(CWlMinimalServer)"));
    EXPECT_THAT(out, HasSubstr("END_REQUEST_MAP()"));
}

TEST(RoundtripServer, VersionPropagated) {
    auto proto = parse_protocol_from_string(R"(
<protocol name="ver_test">
  <interface name="wl_ver" version="5"/>
</protocol>)");
    auto out   = generate_server_cxx_header(proto);
    EXPECT_THAT(out, HasSubstr("version        = 5"));
}

TEST(RoundtripServer, DestructorRequestPresent) {
    auto proto = parse_protocol_from_string(R"(
<protocol name="dest_test">
  <interface name="wl_dest_iface" version="1">
    <request name="destroy" type="destructor"/>
  </interface>
</protocol>)");
    auto out   = generate_server_cxx_header(proto);
    EXPECT_THAT(out, HasSubstr("virtual void OnDestroy("));
    EXPECT_THAT(out, HasSubstr("REQUEST_HANDLER("));
}
