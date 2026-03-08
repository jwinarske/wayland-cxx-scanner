// SPDX-License-Identifier: MIT
// Integration test: parse a protocol XML then generate a client header and
// verify the generated code compiles when string-checked against expected
// patterns from the minimal fixture.
#include "codegen_client_cxx.hpp"
#include "xml_parser.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

TEST(RoundtripClient, ParseFixtureAndGenerateHeader) {
    std::error_code ec;
    if (!std::filesystem::exists(fixture_path(), ec))
        GTEST_SKIP() << "fixture not found: " << fixture_path();

    auto proto = parse_protocol(fixture_path());
    auto out   = generate_client_cxx_header(proto);

    // Verify key structural elements are present in the generated output.
    EXPECT_THAT(out, HasSubstr("#pragma once"));
    EXPECT_THAT(out, HasSubstr("namespace minimal::client"));
    EXPECT_THAT(out, HasSubstr("wl_minimal_traits"));
    EXPECT_THAT(out, HasSubstr("CWlMinimal"));
    EXPECT_THAT(out, HasSubstr("wl::CProxyImpl"));
    EXPECT_THAT(out, HasSubstr("void ReqA("));
    EXPECT_THAT(out, HasSubstr("virtual void OnEvtX("));
    EXPECT_THAT(out, HasSubstr("BEGIN_EVENT_MAP(CWlMinimal)"));
    EXPECT_THAT(out, HasSubstr("END_EVENT_MAP()"));
}

TEST(RoundtripClient, VersionPropagated) {
    auto proto = parse_protocol_from_string(R"(
<protocol name="ver_test">
  <interface name="wl_ver" version="7"/>
</protocol>)");
    auto out   = generate_client_cxx_header(proto);
    EXPECT_THAT(out, HasSubstr("version        = 7"));
}

TEST(RoundtripClient, FdArgTypeInClientHeader) {
    auto proto = parse_protocol_from_string(R"(
<protocol name="fd_test">
  <interface name="wl_fd_iface" version="1">
    <request name="send_fd">
      <arg name="the_fd" type="fd"/>
    </request>
  </interface>
</protocol>)");
    auto out   = generate_client_cxx_header(proto);
    EXPECT_THAT(out, HasSubstr("int32_t the_fd"));
}
