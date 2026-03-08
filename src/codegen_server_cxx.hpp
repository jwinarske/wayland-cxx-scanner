// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <string>

namespace wl::scanner {

/// Generate a C++23 server-side header (CRTP resources, traits, request maps).
[[nodiscard]] std::string generate_server_cxx_header(const ir::Protocol& proto);

}  // namespace wl::scanner
