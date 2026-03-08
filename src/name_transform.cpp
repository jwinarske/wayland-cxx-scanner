// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "name_transform.hpp"

#include <algorithm>
#include <cctype>

namespace wl::scanner {

std::string snake_to_pascal(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool cap = true;
    for (char c : s) {
        if (c == '_') {
            cap = true;
        } else {
            out += cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            cap = false;
        }
    }
    return out;
}

std::string snake_to_camel(std::string_view s) {
    std::string out = snake_to_pascal(s);
    if (!out.empty())
        out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
    return out;
}

std::string strip_prefix_pascal(std::string_view name, std::string_view prefix) {
    // Try to remove the prefix (with trailing underscore) case-insensitively.
    if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix &&
        name[prefix.size()] == '_') {
        return snake_to_pascal(name.substr(prefix.size() + 1));
    }
    return snake_to_pascal(name);
}

std::string enum_entry_to_pascal(std::string_view entry_name, std::string_view enum_name) {
    // Remove common prefix shared with the enum name.
    return strip_prefix_pascal(entry_name, enum_name);
}

}  // namespace wl::scanner
