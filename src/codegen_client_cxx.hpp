// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <string>

namespace wl::scanner {

/// Generate a client-side header (CRTP proxies, traits, event maps).
/// @param proto  The parsed protocol IR.
/// @param std    Target C++ standard; controls which language features are
///               emitted in the generated header (default: C++17).
/// @param emit_interface_tables  When true, also emit inline wl_interface
///               tables and define each traits' wl_iface() in the header, so it
///               is self-contained for an extension protocol with no
///               libwayland-provided interface symbol (default: false).
[[nodiscard]] std::string generate_client_cxx_header(
    const ir::Protocol& proto,
    CppStd std = CppStd::Cpp17,
    bool emit_interface_tables = false);

}  // namespace wl::scanner
