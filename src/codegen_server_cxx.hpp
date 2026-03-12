// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <string>

namespace wl::scanner {

/// Generate a server-side header (CRTP resources, traits, request maps).
/// @param proto  The parsed protocol IR.
/// @param std    Target C++ standard; controls which language features are
///               emitted in the generated header (default: C++23).
[[nodiscard]] std::string generate_server_cxx_header(
    const ir::Protocol& proto, CppStd std = CppStd::Cpp23);

}  // namespace wl::scanner
