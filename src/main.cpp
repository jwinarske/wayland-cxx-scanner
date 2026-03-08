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

void print_usage(std::string_view argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [--mode=<mode>] <protocol.xml> [<output.hpp>]\n"
      "\n"
      "Modes:\n"
      "  client-header   Generate C++23 client proxy header (default)\n"
      "  server-header   Generate C++23 server resource header\n"
      "  c-header        Generate C-style protocol header\n"
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

  Mode mode = Mode::ClientHeader;
  const char* input_path = nullptr;
  const char* output_path = nullptr;

  for (std::size_t i = 1; i < args.size(); ++i) {
    std::string_view arg{args[i]};

    if (arg.starts_with("--mode=")) {
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
    auto proto = wl::scanner::parse_protocol(input_path);

    std::string output;
    switch (mode) {
      case Mode::ClientHeader:
        output = wl::scanner::generate_client_cxx_header(proto);
        break;
      case Mode::ServerHeader:
        output = wl::scanner::generate_server_cxx_header(proto);
        break;
      case Mode::CHeader:
        output = wl::scanner::generate_c_header(proto);
        break;
    }

    if (output_path) {
      std::ofstream ofs(output_path);
      if (!ofs) {
        std::fprintf(stderr, "error: cannot open output file '%s'\n",
                     output_path);
        return EXIT_FAILURE;
      }
      ofs << output;
      // R4: flush and check for write errors (e.g. disk full).
      ofs.flush();
      if (ofs.fail()) {
        std::fprintf(stderr, "error: write failed on '%s'\n", output_path);
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
