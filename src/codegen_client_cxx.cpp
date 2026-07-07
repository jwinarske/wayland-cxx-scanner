// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "codegen_client_cxx.hpp"

#include "codegen_interface_tables.hpp"
#include "name_transform.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace wl::scanner {

using namespace ir;

namespace {

/// Parse the optional since string; default to 1 when absent.
uint32_t msg_since(const Message& m) {
  return m.since.empty() ? 1u : static_cast<uint32_t>(std::stoul(m.since));
}

/// Emit the preprocessor since-version macros for one interface (mirrors the C
/// output in codegen_c.cpp): IFACE_INTERFACE_VERSION and, per request/event,
/// IFACE_<MSG>_SINCE_VERSION. These let a consumer gate version-dependent code
/// with the preprocessor — `#ifdef <IFACE>_<EVENT>_SINCE_VERSION` to override
/// an event only on builds whose XML carries it, and `<bound> >=
/// <...>_SINCE_VERSION` to gate a version-added request — which the constexpr
/// Since constants below cannot do.
void emit_since_versions(std::ostringstream& os, const Interface& iface) {
  const std::string prefix = to_upper(iface.name) + "_";
  os << "#define " << prefix << "INTERFACE_VERSION " << iface.version << "\n";
  for (const auto& r : iface.requests)
    os << "#define " << prefix << to_upper(r.name) << "_SINCE_VERSION "
       << msg_since(r) << "\n";
  for (const auto& e : iface.events)
    os << "#define " << prefix << to_upper(e.name) << "_SINCE_VERSION "
       << msg_since(e) << "\n";
  os << "\n";
}

/// Direction of the message an argument belongs to.  The C++ type of a
/// `new_id` argument depends on it: in a request the client supplies the id (a
/// `uint32_t` consumed by `construct<>` / `_MarshalNew`), but in an event
/// libwayland has already constructed the object and passes the listener a
/// pointer-width `wl_proxy*` — mapping it to `uint32_t` would read 4 bytes of
/// an 8-byte pointer on LP64 (a C-ABI mismatch / UB).
enum class MsgDir { Request, Event };

/// Map ArgType to a C++ parameter type string, given the message direction.
/// M3: every ArgType value is explicitly listed; the default branch is
/// unreachable (asserts in debug, returns void* in release as a last resort
/// to avoid silently emitting wrong code when a new ArgType is added).
std::string_view cpp_arg_type(const Arg& arg, MsgDir dir) {
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
      return "wl_proxy*";
    case ArgType::NewId:
      // Event: libwayland passes the object it constructed as a typed pointer;
      // the slot must be pointer-width.  Request: the id the client chooses.
      return dir == MsgDir::Event ? "wl_proxy*" : "uint32_t";
    case ArgType::Array:
      return "wl_array*";
    case ArgType::Fd:
      return "int32_t";
    case ArgType::Enum:
      return "uint32_t";
  }
  assert(false && "unhandled ArgType in cpp_arg_type");
  return "void*";
}

/// Expression that reads argument @p idx of an EVENT out of libwayland's
/// `union wl_argument args[]`, converted to the same C++ type cpp_arg_type()
/// emits for the virtual On* handler.  Used by the add_dispatcher path: unlike
/// the add_listener trampolines (where libwayland+libffi demarshal into typed
/// C params for us), a dispatcher receives the raw union and must pick the
/// right member per arg type.
std::string wl_arg_read(const Arg& arg, std::size_t idx) {
  const std::string a = "args[" + std::to_string(idx) + "].";
  switch (arg.type) {
    case ArgType::Int:
      return a + "i";
    case ArgType::Uint:
    case ArgType::Enum:
      return a + "u";
    case ArgType::Fixed:
      return a + "f";
    case ArgType::String:
      return a + "s";
    case ArgType::Object:
    case ArgType::NewId:
      // In an event libwayland has already constructed/resolved the object and
      // stores it in the `.o` slot as wl_object* (== wl_proxy* for a client
      // proxy); cpp_arg_type() types the handler param as wl_proxy*.
      return "reinterpret_cast<wl_proxy*>(" + a + "o)";
    case ArgType::Array:
      return a + "a";
    case ArgType::Fd:
      return a + "h";
  }
  assert(false && "unhandled ArgType in wl_arg_read");
  return "{}";
}

/// True if the request has a dynamic new_id — a <arg type="new_id"> with no
/// `interface` attribute, i.e. the wl_registry.bind pattern where the target
/// interface is chosen at runtime.  Such a request cannot be marshaled like a
/// normal one (its new_id expands on the wire to interface-name + version +
/// id); it needs the versioned dynamic-bind path (_MarshalBind / wl::bind).
bool has_dynamic_bind(const Message& r) {
  return std::any_of(r.args.begin(), r.args.end(), [](const Arg& a) {
    return a.type == ArgType::NewId && a.interface_name.empty();
  });
}

/// Emit a dynamic-bind request method (e.g. wl_registry.bind): a member
/// template parameterized on the target BindTraits, delegating to _MarshalBind.
/// The generated signature drops the dynamic new_id arg and appends a synthetic
/// `version` parameter (the client-chosen interface version, as in libwayland's
/// wl_registry_bind).  @p deducing_this selects the C++23 explicit-object form
/// (`self`) vs. the C++17/20 `Base::` form.
void emit_bind_request(std::ostringstream& os,
                       const Message& r,
                       std::string_view traits_name,
                       bool deducing_this) {
  const std::string method = snake_to_pascal(r.name);
  os << "  template <typename BindTraits>\n";
  os << "  [[nodiscard]] wl_proxy* " << method << "(";
  if (deducing_this)
    os << "this Derived& self, ";
  for (const auto& a : r.args)
    if (a.type != ArgType::NewId)
      os << cpp_arg_type(a, MsgDir::Request) << " " << a.name << ", ";
  os << "uint32_t version) noexcept {\n";
  os << "    return "
     << (deducing_this ? "self._MarshalBind(" : "Base::_MarshalBind(")
     << traits_name << "::Op::" << method
     << ", &BindTraits::wl_iface(), version";
  for (const auto& a : r.args)
    if (a.type != ArgType::NewId)
      os << ", " << a.name;
  os << ");\n  }\n\n";
}

void emit_enum(std::ostringstream& os, const Interface& iface, const Enum& en) {
  // S6: names validated at parse time.
  os << "enum class " << snake_to_pascal(iface.name) << snake_to_pascal(en.name)
     << " : uint32_t {\n";
  for (const auto& e : en.entries)
    os << "  " << enum_entry_to_pascal(e.name, en.name) << " = " << e.value
       << ",\n";
  os << "};\n\n";
}

void emit_traits(std::ostringstream& os,
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
    os << "  struct Op {\n";
    for (const auto& r : iface.requests)
      os << "    static constexpr uint32_t " << snake_to_pascal(r.name) << " = "
         << r.opcode << ";\n";
    // Since-version constants nested inside Op (one per request).
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
    // Since-version constants nested inside Evt (one per event).
    os << "    struct Since {\n";
    for (const auto& e : iface.events)
      os << "      static constexpr uint32_t " << snake_to_pascal(e.name)
         << " = " << msg_since(e) << ";\n";
    os << "    };\n";
    os << "  };\n";
  }
  os << "};\n\n";
}

void emit_client_class(std::ostringstream& os,
                       const Interface& iface,
                       CppStd std,
                       std::string_view traits_name) {
  // S6: names validated at parse time.
  std::string cls_name = "C" + snake_to_pascal(iface.name);

  os << "template <class Derived>\n";
  // C++20+ adds a requires-constraint to catch non-class template arguments
  // at instantiation time with a clearer diagnostic.
  if (std >= CppStd::Cpp20)
    os << "  requires std::is_class_v<Derived>\n";
  os << "class " << cls_name << " : public wl::CProxyImpl<Derived, "
     << traits_name << "> {\n";

  // C++23 can use the explicit-object parameter ("deducing this", P0847R7)
  // when the compiler supports it (__cpp_explicit_this_parameter, GCC 14+,
  // Clang 18+).  On older compilers that only partially implement C++23 (e.g.
  // GCC 13 which sets __cplusplus=202100L, not 202302L), we fall back to the
  // C++17/20 `using Base` + `Base::method()` form.
  if (std >= CppStd::Cpp23) {
    // Emit a compile-time feature switch so downstream consumers work with
    // both GCC 13 (no deducing-this) and GCC 14+ / Clang 18+ (deducing-this).
    os << "#ifdef __cpp_explicit_this_parameter\n";
    os << " public:\n";
    for (const auto& r : iface.requests) {
      if (has_dynamic_bind(r)) {
        emit_bind_request(os, r, traits_name, /*deducing_this=*/true);
        continue;
      }
      std::string method = snake_to_pascal(r.name);
      // C++23: explicit-object parameter (P0847R7).
      os << "  void " << method << "(this Derived& self";
      for (const auto& a : r.args)
        os << ", " << cpp_arg_type(a, MsgDir::Request) << " " << a.name;
      os << ") noexcept {\n";
      os << "    self._Marshal(" << traits_name << "::Op::" << method;
      for (const auto& a : r.args)
        os << ", " << a.name;
      if (r.is_destructor)
        os << ");\n    wl_proxy_destroy(self.Detach());\n  }\n\n";
      else
        os << ");\n  }\n\n";
    }
    os << "#else\n";
    os << "  using Base = wl::CProxyImpl<Derived, " << traits_name << ">;\n";
    os << "\n public:\n";
    for (const auto& r : iface.requests) {
      if (has_dynamic_bind(r)) {
        emit_bind_request(os, r, traits_name, /*deducing_this=*/false);
        continue;
      }
      std::string method = snake_to_pascal(r.name);
      // Fallback: traditional Base:: form.
      os << "  void " << method << "(";
      for (std::size_t i = 0; i < r.args.size(); ++i) {
        if (i > 0)
          os << ", ";
        const auto& arg = r.args.at(i);
        os << cpp_arg_type(arg, MsgDir::Request) << " " << arg.name;
      }
      os << ") noexcept {\n";
      os << "    Base::_Marshal(" << traits_name << "::Op::" << method;
      for (const auto& a : r.args)
        os << ", " << a.name;
      if (r.is_destructor)
        os << ");\n    wl_proxy_destroy(Base::Detach());\n  }\n\n";
      else
        os << ");\n  }\n\n";
    }
    os << "#endif\n\n";
  } else {
    // C++17/20: traditional form using the Base alias.
    os << "  using Base = wl::CProxyImpl<Derived, " << traits_name << ">;\n\n";
    os << " public:\n";
    for (const auto& r : iface.requests) {
      if (has_dynamic_bind(r)) {
        emit_bind_request(os, r, traits_name, /*deducing_this=*/false);
        continue;
      }
      std::string method = snake_to_pascal(r.name);
      os << "  void " << method << "(";
      for (std::size_t i = 0; i < r.args.size(); ++i) {
        if (i > 0)
          os << ", ";
        const auto& arg = r.args.at(i);
        os << cpp_arg_type(arg, MsgDir::Request) << " " << arg.name;
      }
      os << ") noexcept {\n";
      os << "    Base::_Marshal(" << traits_name << "::Op::" << method;
      for (const auto& a : r.args)
        os << ", " << a.name;
      if (r.is_destructor)
        os << ");\n    wl_proxy_destroy(Base::Detach());\n  }\n\n";
      else
        os << ");\n  }\n\n";
    }
  }

  // Virtual event handlers — users override these in their Derived class.
  for (const auto& e : iface.events) {
    os << "  virtual void On" << snake_to_pascal(e.name) << "(";
    for (std::size_t i = 0; i < e.args.size(); ++i) {
      if (i > 0)
        os << ", ";
      const auto& arg = e.args.at(i);
      os << cpp_arg_type(arg, MsgDir::Event) << " /*" << arg.name << "*/";
    }
    os << ") {}\n";
  }

  if (!iface.events.empty()) {
    os << "\n private:\n";
    // Allow the CRTP base (_SetProxy) to reach the private dispatcher.
    os << "  friend class wl::CProxyImpl<Derived, " << traits_name << ">;\n\n";

    // Single event dispatcher installed via wl_proxy_add_dispatcher (NOT
    // add_listener).  It receives the raw `union wl_argument args[]` and
    // demarshals by opcode, then calls the virtual On* handler.  Using a
    // dispatcher leaves the proxy's user_data free for the consumer to own —
    // add_listener would overwrite it with the handler pointer, breaking host
    // toolkits that reverse-map proxy->object via wl_proxy_get_user_data.
    os << "  static int _Dispatch(const void* impl, void* /*target*/,\n";
    os << "                       uint32_t opcode, const wl_message* "
          "/*msg*/,\n";
    os << "                       wl_argument* args) {\n";
    os << "    auto* self = static_cast<" << cls_name
       << "*>(const_cast<void*>(impl));\n";
    os << "    (void)args;\n";
    os << "    switch (opcode) {\n";
    for (std::size_t ei = 0; ei < iface.events.size(); ++ei) {
      const auto& e = iface.events.at(ei);
      os << "      case " << ei << ":\n";
      os << "        self->On" << snake_to_pascal(e.name) << "(";
      for (std::size_t i = 0; i < e.args.size(); ++i) {
        if (i > 0)
          os << ", ";
        os << wl_arg_read(e.args.at(i), i);
      }
      os << ");\n";
      os << "        break;\n";
    }
    os << "      default:\n";
    os << "        break;\n";
    os << "    }\n";
    os << "    return 0;\n";
    os << "  }\n";
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

std::string generate_client_cxx_header(const Protocol& proto,
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
  os << "#include <wl/proxy_impl.hpp>\n";
  os << "\n";
  os << "#include <cstdint>\n";
  os << "#include <string_view>\n";
  // <type_traits> is needed for the std::is_class_v requires-constraint
  // (C++20+).
  if (std >= CppStd::Cpp20)
    os << "#include <type_traits>\n";
  os << "\n";

  // Since-version macros at file scope (before the namespace) so consumers can
  // gate version-dependent handlers/requests with the preprocessor.
  for (const auto& iface : proto.interfaces)
    emit_since_versions(os, iface);

  std::string ns = proto.name + "::client";
  os << "namespace " << ns << " {\n\n";

  for (const auto& iface : proto.interfaces) {
    for (const auto& en : iface.enums)
      emit_enum(os, iface, en);
    const std::string traits_name = iface.name + "_traits";
    emit_traits(os, iface, std, traits_name);
    emit_client_class(os, iface, std, traits_name);
  }

  if (emit_interface_tables)
    wl::scanner::emit_interface_tables(os, proto, "_traits");

  os << "}  // namespace " << ns << "\n";
  os << "// clang-format on\n";
  return std::move(os).str();
}

}  // namespace wl::scanner
