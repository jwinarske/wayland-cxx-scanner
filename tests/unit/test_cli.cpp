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
using ::testing::Not;

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

// ── C++ standard selection tests
// ──────────────────────────────────────────

// C++17: no requires-constraint, no [[nodiscard("reason")]], has "using Base"
TEST(CppStd, ClientHeaderCpp17HasNoRequires) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_client_cxx_header(proto, CppStd::Cpp17);
  EXPECT_THAT(out, HasSubstr("// Target: C++17"));
  // No __cplusplus guard — GCC 13 sets __cplusplus=202100L even with -std=c++23.
  EXPECT_THAT(out, Not(HasSubstr("#if __cplusplus")));
  EXPECT_THAT(out, Not(HasSubstr("requires std::is_class_v")));
  EXPECT_THAT(out, Not(HasSubstr("[[nodiscard(\"required for protocol binding\")]]")));
  EXPECT_THAT(out, HasSubstr("[[nodiscard]] static const wl_interface"));
  EXPECT_THAT(out, HasSubstr("using Base = wl::CProxyImpl<Derived"));
  EXPECT_THAT(out, HasSubstr("Base::_Marshal("));
}

// C++20: has requires-constraint, [[nodiscard("reason")]], and "using Base"
TEST(CppStd, ClientHeaderCpp20HasRequires) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_client_cxx_header(proto, CppStd::Cpp20);
  EXPECT_THAT(out, HasSubstr("// Target: C++20"));
  EXPECT_THAT(out, Not(HasSubstr("#if __cplusplus")));
  EXPECT_THAT(out, HasSubstr("requires std::is_class_v<Derived>"));
  EXPECT_THAT(out, HasSubstr("#include <type_traits>"));
  EXPECT_THAT(out, HasSubstr("[[nodiscard(\"required for protocol binding\")]]"));
  EXPECT_THAT(out, HasSubstr("using Base = wl::CProxyImpl<Derived"));
  EXPECT_THAT(out, HasSubstr("Base::_Marshal("));
}

// C++23: has requires, [[nodiscard("reason")]], feature-guarded deducing-this.
TEST(CppStd, ClientHeaderCpp23HasDeducingThis) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_client_cxx_header(proto, CppStd::Cpp23);
  EXPECT_THAT(out, HasSubstr("// Target: C++23"));
  EXPECT_THAT(out, Not(HasSubstr("#if __cplusplus")));
  EXPECT_THAT(out, HasSubstr("requires std::is_class_v<Derived>"));
  EXPECT_THAT(out, HasSubstr("[[nodiscard(\"required for protocol binding\")]]"));
  // Deducing-this is guarded by __cpp_explicit_this_parameter for portability
  // across GCC 13 (which sets __cplusplus=202100L) and GCC 14+ / Clang 18+.
  EXPECT_THAT(out, HasSubstr("#ifdef __cpp_explicit_this_parameter"));
  EXPECT_THAT(out, HasSubstr("this Derived& self"));
  EXPECT_THAT(out, HasSubstr("self._Marshal("));
  // Fallback path still has "using Base" for compilers without deducing-this.
  EXPECT_THAT(out, HasSubstr("using Base = wl::CProxyImpl<Derived"));
}

// C++23 is the default when no standard is specified.
TEST(CppStd, ClientHeaderDefaultIsCpp23) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out_default = generate_client_cxx_header(proto);
  auto out_cpp23 = generate_client_cxx_header(proto, CppStd::Cpp23);
  EXPECT_EQ(out_default, out_cpp23);
}

// Server header: C++17 has no requires, C++20+ has requires, C++23 uses deducing-this.
TEST(CppStd, ServerHeaderCpp17HasNoRequires) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_server_cxx_header(proto, CppStd::Cpp17);
  EXPECT_THAT(out, HasSubstr("// Target: C++17"));
  EXPECT_THAT(out, Not(HasSubstr("#if __cplusplus")));
  EXPECT_THAT(out, Not(HasSubstr("requires std::is_class_v")));
  EXPECT_THAT(out, HasSubstr("using Base = wl::CResourceImpl<Derived"));
  EXPECT_THAT(out, HasSubstr("Base::_PostEvent("));
}

TEST(CppStd, ServerHeaderCpp20HasRequires) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_server_cxx_header(proto, CppStd::Cpp20);
  EXPECT_THAT(out, HasSubstr("// Target: C++20"));
  EXPECT_THAT(out, Not(HasSubstr("#if __cplusplus")));
  EXPECT_THAT(out, HasSubstr("requires std::is_class_v<Derived>"));
  EXPECT_THAT(out, HasSubstr("#include <type_traits>"));
  EXPECT_THAT(out, HasSubstr("using Base = wl::CResourceImpl<Derived"));
  EXPECT_THAT(out, HasSubstr("Base::_PostEvent("));
}

TEST(CppStd, ServerHeaderCpp23HasDeducingThis) {
  auto proto = parse_protocol_from_string(fixture_xml());
  auto out = generate_server_cxx_header(proto, CppStd::Cpp23);
  EXPECT_THAT(out, HasSubstr("// Target: C++23"));
  EXPECT_THAT(out, Not(HasSubstr("#if __cplusplus")));
  EXPECT_THAT(out, HasSubstr("requires std::is_class_v<Derived>"));
  // Deducing-this is guarded by __cpp_explicit_this_parameter for portability.
  EXPECT_THAT(out, HasSubstr("#ifdef __cpp_explicit_this_parameter"));
  EXPECT_THAT(out, HasSubstr("this Derived& self"));
  EXPECT_THAT(out, HasSubstr("self._PostEvent("));
  // Fallback path still has "using Base" for compilers without deducing-this.
  EXPECT_THAT(out, HasSubstr("using Base = wl::CResourceImpl<Derived"));
}

