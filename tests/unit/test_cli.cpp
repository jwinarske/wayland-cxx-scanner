// SPDX-License-Identifier: MIT
// CLI integration tests — exercise the scanner's argument parsing and
// end-to-end invocation using the XML parser + code generators.
#include "codegen_c.hpp"
#include "codegen_client_cxx.hpp"
#include "codegen_server_cxx.hpp"
#include "xml_parser.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace wl::scanner;
using ::testing::HasSubstr;

// ── Helpers
// ───────────────────────────────────────────────────────────────────

static std::string fixture_xml() {
  return R"(<protocol name="cli_test">
  <interface name="wl_cli" version="1">
    <request name="do_it">
      <arg name="val" type="uint"/>
    </request>
    <event name="done">
      <arg name="serial" type="uint"/>
    </event>
  </interface>
</protocol>)";
}

// ── Tests
// ─────────────────────────────────────────────────────────────────────

TEST(Cli, ParseAndGenerateClientHeader) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_client_cxx_header(proto);
  EXPECT_THAT(out, HasSubstr("namespace cli_test::client"));
  EXPECT_THAT(out, HasSubstr("CWlCli"));
}

TEST(Cli, ParseAndGenerateServerHeader) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_server_cxx_header(proto);
  EXPECT_THAT(out, HasSubstr("namespace cli_test::server"));
  EXPECT_THAT(out, HasSubstr("CWlCliServer"));
}

TEST(Cli, ParseAndGenerateCHeader) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_c_header(proto);
  EXPECT_THAT(out, HasSubstr("wl_cli_interface"));
  EXPECT_THAT(out, HasSubstr("WL_CLI_DO_IT 0"));
}

TEST(Cli, InvalidXmlReturnsParseError) {
  EXPECT_THROW((void)parse_protocol_from_string("<<<"), ir::ParseError);
}

TEST(Cli, WriteAndReadTempFile) {
  namespace fs = std::filesystem;
  fs::path tmp = fs::temp_directory_path() / "wl_cli_test.xml";
  {
    std::ofstream ofs(tmp);
    ofs << fixture_xml();
  }
  auto proto = parse_protocol(tmp);
  EXPECT_EQ(proto.name, "cli_test");
  fs::remove(tmp);
}
