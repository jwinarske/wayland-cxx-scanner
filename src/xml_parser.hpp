// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <filesystem>
#include <string_view>

namespace wl::scanner {

using ir::ParseError;
using ir::Protocol;

/// Parse a Wayland XML protocol from the given file path.
/// @throws std::system_error if the file cannot be opened.
/// @throws ParseError        if the XML is malformed or contains unknown types.
[[nodiscard]] Protocol parse_protocol(const std::filesystem::path& path);

/// Parse a Wayland XML protocol from an in-memory string.
/// @throws ParseError if the XML is malformed or contains unknown types.
[[nodiscard]] Protocol parse_protocol_from_string(std::string_view xml);

}  // namespace wl::scanner
