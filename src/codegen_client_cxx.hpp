// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <string>

namespace wl::scanner {

/// Generate a client-side header (CRTP proxies, traits, event maps).
/// @param proto  The parsed protocol IR.
/// @param std    Target C++ standard; controls which language features are
///               emitted in the generated header (default: C++23).
[[nodiscard]] std::string generate_client_cxx_header(
    const ir::Protocol& proto,
    CppStd std = CppStd::Cpp23);

}  // namespace wl::scanner
