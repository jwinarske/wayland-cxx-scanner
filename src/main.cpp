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
#include <string_view>

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [--mode=<mode>] <protocol.xml> [<output.hpp>]\n"
                 "\n"
                 "Modes:\n"
                 "  client-header   Generate C++23 client proxy header (default)\n"
                 "  server-header   Generate C++23 server resource header\n"
                 "  c-header        Generate C-style protocol header\n"
                 "\n"
                 "If <output.hpp> is omitted, the output is written to stdout.\n",
                 argv0);
}

enum class Mode { ClientHeader, ServerHeader, CHeader };

}  // anonymous namespace

int main(int argc, char** argv) {
    Mode        mode       = Mode::ClientHeader;
    const char* input_path = nullptr;
    const char* output_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

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
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (!input_path) {
            input_path = argv[i];
        } else if (!output_path) {
            output_path = argv[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!input_path) {
        std::fprintf(stderr, "error: no input file specified\n");
        print_usage(argv[0]);
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
                std::fprintf(stderr, "error: cannot open output file '%s'\n", output_path);
                return EXIT_FAILURE;
            }
            ofs << output;
        } else {
            std::cout << output;
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
