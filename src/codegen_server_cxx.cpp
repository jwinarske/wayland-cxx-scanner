// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "codegen_server_cxx.hpp"

#include "codegen_interface_tables.hpp"
#include "name_transform.hpp"

#include <cassert>
#include <sstream>

namespace wl::scanner {

using namespace ir;

namespace {

/// Parse the optional since string; default to 1 when absent.
uint32_t msg_since(const Message& m) {
  return m.since.empty() ? 1u : static_cast<uint32_t>(std::stoul(m.since));
}

/// Map ArgType to a C++ parameter type for server-side code.
/// M3: every ArgType value is explicitly listed; the default branch is
/// unreachable and asserts in debug builds.
std::string_view cpp_server_arg_type(const Arg& arg) {
  switch (arg.type) {
    case ArgType::Int:
      return "int32_t";
    case ArgType::Uint:
      return "uint32_t";
    case ArgType::Fixed:
      return "wl_fixed_t";
    case ArgType::String:
      return "const char*";
    case ArgType::Object:
      return "wl_resource*";
    case ArgType::NewId:
      return "uint32_t";
    case ArgType::Array:
      return "wl_array*";
    case ArgType::Fd:
      return "int32_t";
    case ArgType::Enum:
      return "uint32_t";
  }
  assert(false && "unhandled ArgType in cpp_server_arg_type");
  return "void*";
}

void emit_server_traits(std::ostringstream& os,
                        const Interface& iface,
                        CppStd std,
                        std::string_view traits_name) {
  os << "struct " << traits_name << " {\n";
  os << "  static constexpr std::string_view interface_name = \"" << iface.name
     << "\";\n";
  os << "  static constexpr uint32_t version = " << iface.version << ";\n";
  // C++20+ supports [[nodiscard]] with a reason string.
  if (std >= CppStd::Cpp20)
    os << "  [[nodiscard(\"required for protocol binding\")]]\n"
       << "  static const wl_interface& wl_iface() noexcept;\n";
  else
    os << "  [[nodiscard]] static const wl_interface& wl_iface() noexcept;\n";

  if (!iface.requests.empty()) {
    os << "  struct Req {\n";
    for (const auto& r : iface.requests)
      os << "    static constexpr uint32_t " << snake_to_pascal(r.name) << " = "
         << r.opcode << ";\n";
    os << "    struct Since {\n";
    for (const auto& r : iface.requests)
      os << "      static constexpr uint32_t " << snake_to_pascal(r.name)
         << " = " << msg_since(r) << ";\n";
    os << "    };\n";
    os << "  };\n";
  }

  if (!iface.events.empty()) {
    os << "  struct Evt {\n";
    for (const auto& e : iface.events)
      os << "    static constexpr uint32_t " << snake_to_pascal(e.name) << " = "
         << e.opcode << ";\n";
    os << "    struct Since {\n";
    for (const auto& e : iface.events)
      os << "      static constexpr uint32_t " << snake_to_pascal(e.name)
         << " = " << msg_since(e) << ";\n";
    os << "    };\n";
    os << "  };\n";
  }
  os << "};\n\n";
}

void emit_server_class(std::ostringstream& os,
                       const Interface& iface,
                       CppStd std) {
  // S6: names validated at parse time.
  std::string cls_name = "C" + snake_to_pascal(iface.name) + "Server";
  std::string traits_name = iface.name + "_server_traits";

  emit_server_traits(os, iface, std, traits_name);

  os << "template <class Derived>\n";
  // C++20+ adds a requires-constraint to catch non-class template arguments
  // at instantiation time with a clearer diagnostic.
  if (std >= CppStd::Cpp20)
    os << "  requires std::is_class_v<Derived>\n";
  os << "class " << cls_name << " : public wl::CResourceImpl<Derived, "
     << traits_name << "> {\n";

  // C++23 can use the explicit-object parameter ("deducing this", P0847R7)
  // when the compiler supports it (__cpp_explicit_this_parameter, GCC 14+,
  // Clang 18+).  On older compilers that only partially implement C++23 (e.g.,
  // GCC 13, which sets __cplusplus=202100L, not 202302L), we fall back to the
  // C++17/20 `using Base` + `Base::method()` form.
  if (std >= CppStd::Cpp23) {
    // Emit a compile-time feature switch so downstream consumers work with
    // both GCC 13 (no deducing-this) and GCC 14+ / Clang 18+ (deducing-this).
    os << "#ifdef __cpp_explicit_this_parameter\n";
    os << " public:\n";
    for (const auto& e : iface.events) {
      std::string method = "Send" + snake_to_pascal(e.name);
      // C++23: explicit-object parameter (P0847R7).
      os << "  void " << method << "(this Derived& self";
      for (const auto& a : e.args)
        os << ", " << cpp_server_arg_type(a) << " " << a.name;
      os << ") noexcept {\n";
      os << "    self._PostEvent(" << traits_name
         << "::Evt::" << snake_to_pascal(e.name);
      for (const auto& a : e.args)
        os << ", " << a.name;
      os << ");\n  }\n\n";
    }
    os << "#else\n";
    os << "  using Base = wl::CResourceImpl<Derived, " << traits_name << ">;\n";
    os << "\n public:\n";
    for (const auto& e : iface.events) {
      std::string method = "Send" + snake_to_pascal(e.name);
      // Fallback: traditional Base:: form.
      os << "  void " << method << "(";
      for (std::size_t i = 0; i < e.args.size(); ++i) {
        if (i > 0)
          os << ", ";
        const auto& arg = e.args.at(i);
        os << cpp_server_arg_type(arg) << " " << arg.name;
      }
      os << ") noexcept {\n";
      os << "    Base::_PostEvent(" << traits_name
         << "::Evt::" << snake_to_pascal(e.name);
      for (const auto& a : e.args)
        os << ", " << a.name;
      os << ");\n  }\n\n";
    }
    os << "#endif\n\n";
  } else {
    // C++17/20: traditional form using the Base alias.
    os << "  using Base = wl::CResourceImpl<Derived, " << traits_name
       << ">;\n\n";
    os << " public:\n";
    for (const auto& e : iface.events) {
      std::string method = "Send" + snake_to_pascal(e.name);
      os << "  void " << method << "(";
      for (std::size_t i = 0; i < e.args.size(); ++i) {
        if (i > 0)
          os << ", ";
        const auto& arg = e.args.at(i);
        os << cpp_server_arg_type(arg) << " " << arg.name;
      }
      os << ") noexcept {\n";
      os << "    Base::_PostEvent(" << traits_name
         << "::Evt::" << snake_to_pascal(e.name);
      for (const auto& a : e.args)
        os << ", " << a.name;
      os << ");\n  }\n\n";
    }
  }

  // Virtual request handlers — users override these in their Derived class.
  for (const auto& r : iface.requests) {
    os << "  virtual void On" << snake_to_pascal(r.name)
       << "(wl_client* /*client*/, wl_resource* /*resource*/";
    for (const auto& a : r.args)
      os << ", " << cpp_server_arg_type(a) << " /*" << a.name << "*/";
    os << ") {}\n";
  }

  if (!iface.requests.empty()) {
    os << "\n private:\n";
    // Allow the CRTP base to access the private request vtable.
    os << "  friend class wl::CResourceImpl<Derived, " << traits_name
       << ">;\n\n";

    // Direct-dispatch static callbacks — call OnFoo directly without
    // intermediate ProcessRequest / opcode scan.
    for (const auto& r : iface.requests) {
      // R5: generated dispatch functions null-check the user_data pointer
      // before dereferencing, guarding against uninitialized resources.
      // Forwarded arguments use positional names (a0, a1, ...) so they can
      // never collide with the fixed `client` / `resource` / `self` names,
      // regardless of the protocol's argument names.
      os << "  static void _Req" << snake_to_pascal(r.name)
         << "(wl_client* client, wl_resource* resource";
      for (std::size_t i = 0; i < r.args.size(); ++i)
        os << ", " << cpp_server_arg_type(r.args.at(i)) << " a" << i;
      os << ") {\n";
      os << "    auto* self = static_cast<" << cls_name
         << "*>(wl_resource_get_user_data(resource));\n";
      os << "    if (!self)\n      return;  // R5: guard against uninitialized "
            "resource\n";
      os << "    self->On" << snake_to_pascal(r.name) << "(client, resource";
      for (std::size_t i = 0; i < r.args.size(); ++i)
        os << ", a" << i;
      os << ");\n";
      os << "  }\n";
    }
    // reinterpret_cast is not a constant expression, so we cannot use
    // constexpr here.  The inline keyword makes the definition valid inside
    // the class body for all C++17+.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
    os << "  inline static const void* s_request_vtable_[] = {\n";
    for (const auto& r : iface.requests)
      os << "      reinterpret_cast<const void*>(&_Req"
         << snake_to_pascal(r.name) << "),\n";
    os << "  };\n";
  }

  os << "};\n\n";
}

/// Return the C++ standard label string.
const char* cpp_std_label(CppStd std) {
  switch (std) {
    case CppStd::Cpp17:
      return "C++17";
    case CppStd::Cpp20:
      return "C++20";
    case CppStd::Cpp23:
      return "C++23";
  }
  assert(false && "unhandled CppStd");
  return "C++17";
}

}  // anonymous namespace

std::string generate_server_cxx_header(const Protocol& proto,
                                       CppStd std,
                                       bool emit_interface_tables) {
  // S6: all identifiers validated at parse time; safe to emit directly.
  std::ostringstream os;

  const char* cpp_label = cpp_std_label(std);

  os << "// SPDX-License-Identifier: MIT\n";
  os << "// AUTO-GENERATED by wayland-cxx-scanner 0.1.0 — DO NOT EDIT\n";
  os << "// Target: " << cpp_label << "\n";
  // This file is machine-generated; tell clang-format to leave it untouched.
  os << "// clang-format off\n";
  os << "#pragma once\n\n";
  os << "#include <wl/resource_impl.hpp>\n";
  os << "\n";
  os << "#include <cstdint>\n";
  os << "#include <string_view>\n";
  // <type_traits> is needed for the std::is_class_v requires-constraint
  // (C++20+).
  if (std >= CppStd::Cpp20)
    os << "#include <type_traits>\n";
  os << "\n";

  std::string ns = proto.name + "::server";
  os << "namespace " << ns << " {\n\n";

  for (const auto& iface : proto.interfaces)
    emit_server_class(os, iface, std);

  if (emit_interface_tables)
    wl::scanner::emit_interface_tables(os, proto, "_server_traits");

  os << "}  // namespace " << ns << "\n";
  os << "// clang-format on\n";
  return std::move(os).str();
}

}  // namespace wl::scanner
