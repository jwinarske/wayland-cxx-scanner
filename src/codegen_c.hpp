// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <string>

namespace wl::scanner {

/// Generate a C-style protocol header (enums, opcodes, interface declarations).
/// The output is a complete, self-contained header file.
[[nodiscard]] std::string generate_c_header(const ir::Protocol& proto);

}  // namespace wl::scanner
