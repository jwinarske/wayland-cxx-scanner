// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "codegen_c.hpp"
#include "codegen_client_cxx.hpp"
#include "codegen_server_cxx.hpp"
#include "xml_parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>

namespace {

void print_usage(const std::string_view argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [--mode=<mode>] [--std=<std>] <protocol.xml> [<output.hpp>]\n"
      "\n"
      "Modes:\n"
      "  client-header   Generate C++ client proxy header (default)\n"
      "  server-header   Generate C++ server resource header\n"
      "  c-header        Generate C-style protocol header\n"
      "\n"
      "C++ standards (for client-header and server-header):\n"
      "  c++17           ISO C++17 — CRTP without requires-expressions\n"
      "  c++20           ISO C++20 — adds requires-constraints and "
      "[[nodiscard(\"reason\")]]\n"
      "  c++23           ISO C++23 — adds explicit-object parameters "
      "(default)\n"
      "\n"
      "If <output.hpp> is omitted, the output is written to stdout.\n",
      argv0.data());
}

enum class Mode { ClientHeader, ServerHeader, CHeader };

}  // anonymous namespace

int main(int argc, char** argv) {
  // Wrap argv in std::span to avoid pointer-arithmetic warnings.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  auto const args =
      std::span<char* const>(argv, static_cast<std::size_t>(argc));

  auto mode = Mode::ClientHeader;
  auto cpp_std = wl::scanner::CppStd::Cpp23;
  const char* input_path = nullptr;
  const char* output_path = nullptr;

  for (std::size_t i = 1; i < args.size(); ++i) {
    if (std::string_view arg{args[i]}; arg.starts_with("--mode=")) {
      std::string_view m = arg.substr(7);
      if (m == "client-header")
        mode = Mode::ClientHeader;
      else if (m == "server-header")
        mode = Mode::ServerHeader;
      else if (m == "c-header")
        mode = Mode::CHeader;
      else {
        std::fprintf(stderr, "error: unknown mode '%s'\n", m.data());
        print_usage(args[0]);
        return EXIT_FAILURE;
      }
    } else if (arg.starts_with("--std=")) {
      std::string_view s = arg.substr(6);
      if (s == "c++17")
        cpp_std = wl::scanner::CppStd::Cpp17;
      else if (s == "c++20")
        cpp_std = wl::scanner::CppStd::Cpp20;
      else if (s == "c++23")
        cpp_std = wl::scanner::CppStd::Cpp23;
      else {
        std::fprintf(stderr, "error: unknown C++ standard '%s'\n", s.data());
        print_usage(args[0]);
        return EXIT_FAILURE;
      }
    } else if (arg == "--help" || arg == "-h") {
      print_usage(args[0]);
      return EXIT_SUCCESS;
    } else if (!input_path) {
      input_path = args[i];
    } else if (!output_path) {
      output_path = args[i];
    } else {
      std::fprintf(stderr, "error: unexpected argument '%s'\n", args[i]);
      print_usage(args[0]);
      return EXIT_FAILURE;
    }
  }

  if (!input_path) {
    std::fprintf(stderr, "error: no input file specified\n");
    print_usage(args[0]);
    return EXIT_FAILURE;
  }

  try {
    // Resolve the input path to an absolute, normalized form so that any
    // embedded ".." segments are collapsed before the path reaches the OS.
    // std::filesystem::canonical() also verifies the file exists.
    std::filesystem::path safe_input = std::filesystem::canonical(input_path);
    if (!std::filesystem::is_regular_file(safe_input)) {
      std::fprintf(stderr, "error: not a regular file: '%s'\n", input_path);
      return EXIT_FAILURE;
    }

    auto proto = wl::scanner::parse_protocol(safe_input.c_str());

    std::string output;
    switch (mode) {
      case Mode::ClientHeader:
        output = wl::scanner::generate_client_cxx_header(proto, cpp_std);
        break;
      case Mode::ServerHeader:
        output = wl::scanner::generate_server_cxx_header(proto, cpp_std);
        break;
      case Mode::CHeader:
        output = wl::scanner::generate_c_header(proto);
        break;
    }

    if (output_path) {
      // weakly_canonical normalizes ".." segments without requiring the output
      // file to already exist, preventing path-traversal via the output arg.
      std::filesystem::path safe_output =
          std::filesystem::weakly_canonical(output_path);
      std::ofstream ofs(safe_output);
      if (!ofs) {
        std::fprintf(stderr, "error: cannot open output file '%s'\n",
                     safe_output.c_str());
        return EXIT_FAILURE;
      }
      ofs << output;
      // R4: flush and check for write errors (e.g. disk full).
      ofs.flush();
      if (ofs.fail()) {
        std::fprintf(stderr, "error: write failed on '%s'\n",
                     safe_output.c_str());
        return EXIT_FAILURE;
      }
    } else {
      std::cout << output;
      // R4: check stdout write errors too.
      std::cout.flush();
      if (std::cout.fail()) {
        std::fprintf(stderr, "error: write to stdout failed\n");
        return EXIT_FAILURE;
      }
    }
  } catch (const wl::scanner::ir::ParseError& e) {
    std::fprintf(stderr, "parse error: %s\n", e.what());
    return EXIT_FAILURE;
  } catch (const std::system_error& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return EXIT_FAILURE;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
