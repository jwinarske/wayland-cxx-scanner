// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <string>

namespace wl::scanner {

/// Generate a C++23 client-side header (CRTP proxies, traits, event maps).
[[nodiscard]] std::string generate_client_cxx_header(const ir::Protocol& proto);

}  // namespace wl::scanner
