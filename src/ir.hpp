// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace wl::scanner {

/// Target C++ standard for generated code.
enum class CppStd : uint8_t {
  Cpp17,  ///< ISO C++17 — no concepts or requires-expressions
  Cpp20,  ///< ISO C++20 — concepts, requires, [[nodiscard("message")]]
  Cpp23,  ///< ISO C++23 — C++20 features plus explicit-object parameters (default)
};

}  // namespace wl::scanner

namespace wl::scanner::ir {

/// Wayland argument type enumeration (matches <arg type="..."> values).
enum class ArgType : uint8_t {
  Int,     ///< int32_t
  Uint,    ///< uint32_t
  Fixed,   ///< wl_fixed_t
  String,  ///< const char*
  Object,  ///< wl_object* / wl_proxy*
  NewId,   ///< newly-created object id
  Array,   ///< wl_array*
  Fd,      ///< file descriptor
  Enum,    ///< uint32_t aliased as a named enum
};

/// One argument to a request or event.
struct Arg {
  std::string name;
  ArgType type{ArgType::Int};
  std::string interface_name;  ///< for Object / NewId (may be empty)
  std::string enum_name;       ///< for Enum ("iface.entry", qualified)
  bool nullable{false};
  bool allow_null{false};
};

/// One entry inside an <enum>.
struct EnumEntry {
  std::string name;
  uint32_t value{0};
  std::string summary;
};

/// An <enum> inside an interface.
struct Enum {
  std::string name;
  std::vector<EnumEntry> entries;
  bool is_bitfield{false};
};

/// A <request> or <event> inside an interface.
struct Message {
  std::string name;
  uint32_t opcode{0};
  std::vector<Arg> args;
  bool is_destructor{false};
  std::string since;
};

/// A <interface> block.
struct Interface {
  std::string name;
  uint32_t version{1};
  std::vector<Message> requests;
  std::vector<Message> events;
  std::vector<Enum> enums;
};

/// The top-level <protocol> element.
struct Protocol {
  std::string name;
  std::vector<Interface> interfaces;
};

/// Thrown when the XML is malformed or uses unknown types.
struct ParseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

}  // namespace wl::scanner::ir
