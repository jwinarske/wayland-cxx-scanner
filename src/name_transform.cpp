// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "name_transform.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace {

/// Spell every character that is a digit as its English word (PascalCase).
/// Non-digit characters are passed through unchanged.
/// "90"  → "NineZero"
/// "180" → "OneEightZero"
/// "270" → "TwoSevenZero"
std::string spell_digits(const std::string_view s) {
  static constexpr std::array<const char*, 10> kWords = {
      "Zero", "One", "Two",   "Three", "Four",
      "Five", "Six", "Seven", "Eight", "Nine",
  };
  std::string out;
  out.reserve(s.size() * 5);
  for (char c : s) {
    if (c >= '0' && c <= '9')
      out += kWords[static_cast<unsigned char>(c) - '0'];
    else
      out += c;
  }
  return out;
}

}  // anonymous namespace

namespace wl::scanner {

std::string snake_to_pascal(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool cap = true;
  for (char c : s) {
    if (c == '_') {
      cap = true;
    } else {
      out +=
          cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
              : c;
      cap = false;
    }
  }
  return out;
}

std::string snake_to_camel(std::string_view s) {
  std::string out = snake_to_pascal(s);
  if (!out.empty())
    out[0] =
        static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
  return out;
}

std::string strip_prefix_pascal(std::string_view name,
                                std::string_view prefix) {
  // Try to remove the prefix (with trailing underscore) case-insensitively.
  if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix &&
      name[prefix.size()] == '_') {
    return snake_to_pascal(name.substr(prefix.size() + 1));
  }
  return snake_to_pascal(name);
}

std::string enum_entry_to_pascal(std::string_view entry_name,
                                 std::string_view enum_name) {
  // Remove common prefix shared with the enum name.
  std::string result = strip_prefix_pascal(entry_name, enum_name);
  // If the result starts with a digit (e.g. "90" from wl_output.transform),
  // spell every digit out as an English word so the identifier is valid C++.
  // "90" → "NineZero", "180" → "OneEightZero", "270" → "TwoSevenZero"
  if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0])))
    result = spell_digits(result);
  return result;
}

}  // namespace wl::scanner
