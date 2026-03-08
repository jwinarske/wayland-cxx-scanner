// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <string>
#include <string_view>

namespace wl::scanner {

/// Convert a snake_case identifier to PascalCase.
/// Example: "xdg_wm_base" → "XdgWmBase"
[[nodiscard]] std::string snake_to_pascal(std::string_view s);

/// Convert a snake_case identifier to camelCase.
/// Example: "xdg_wm_base" → "xdgWmBase"
[[nodiscard]] std::string snake_to_camel(std::string_view s);

/// Strip a known interface prefix from a name, then PascalCase the remainder.
/// Example: strip_prefix("xdg_wm_base_ping", "xdg_wm_base") → "Ping"
[[nodiscard]] std::string strip_prefix_pascal(std::string_view name, std::string_view prefix);

/// Convert an enum entry name to PascalCase (removing a common prefix).
/// Example: error_role → Role
[[nodiscard]] std::string enum_entry_to_pascal(std::string_view entry_name,
                                               std::string_view enum_name);

}  // namespace wl::scanner
