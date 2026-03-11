// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "codegen_client_cxx.hpp"

#include "name_transform.hpp"

#include <cassert>
#include <sstream>

namespace wl::scanner {

using namespace ir;

namespace {

/// Map ArgType to a C++ parameter type string.
/// M3: every ArgType value is explicitly listed; the default branch is
/// unreachable (asserts in debug, returns void* in release as a last resort
/// to avoid silently emitting wrong code when a new ArgType is added).
std::string cpp_arg_type(const Arg& arg) {
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
      return "uint32_t";
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

void emit_enum(std::ostringstream& os, const Interface& iface, const Enum& en) {
  // S6: names validated at parse time.
  std::string enum_class =
      snake_to_pascal(iface.name) + snake_to_pascal(en.name);
  os << "enum class " << enum_class << " : uint32_t {\n";
  for (const auto& e : en.entries) {
    std::string entry_name = enum_entry_to_pascal(e.name, en.name);
    os << "    " << entry_name << " = " << e.value << ",\n";
  }
  os << "};\n\n";
}

void emit_traits(std::ostringstream& os, const Interface& iface) {
  std::string traits_name = iface.name + "_traits";
  os << "struct " << traits_name << " {\n";
  os << "    static constexpr std::string_view interface_name = \""
     << iface.name << "\";\n";
  os << "    static constexpr uint32_t         version        = "
     << iface.version << ";\n";
  os << "    static const wl_interface& wl_iface() noexcept;\n";

  if (!iface.requests.empty()) {
    os << "    struct Op {\n";
    os << "        static constexpr uint32_t";
    bool first = true;
    for (const auto& r : iface.requests) {
      if (!first)
        os << ",";
      os << "\n            " << snake_to_pascal(r.name) << " = " << r.opcode;
      first = false;
    }
    os << ";\n    };\n";
  }

  if (!iface.events.empty()) {
    os << "    struct Evt {\n";
    os << "        static constexpr uint32_t";
    bool first = true;
    for (const auto& e : iface.events) {
      if (!first)
        os << ",";
      os << "\n            " << snake_to_pascal(e.name) << " = " << e.opcode;
      first = false;
    }
    os << ";\n    };\n";
  }
  os << "};\n\n";
}

void emit_crack_event(std::ostringstream& os, const Message& evt) {
  os << "    template <typename T, typename Fn>\n";
  os << "    static void _CrackEvent_" << evt.opcode
     << "(T* self, void** args, Fn fn) {\n";
  if (evt.args.empty()) {
    os << "        (void)args;\n";
    os << "        (self->*fn)();\n";
  } else {
    os << "        (self->*fn)(";
    for (std::size_t i = 0; i < evt.args.size(); ++i) {
      if (i > 0)
        os << ", ";
      os << "*reinterpret_cast<" << cpp_arg_type(evt.args[i]) << "*>(args[" << i
         << "])";
    }
    os << ");\n";
  }
  os << "    }\n\n";
}

void emit_client_class(std::ostringstream& os, const Interface& iface) {
  // S6: names validated at parse time.
  std::string cls_name = "C" + snake_to_pascal(iface.name);
  std::string traits_name = iface.name + "_traits";

  os << "template <class Derived>\n";
  os << "class " << cls_name << " : public wl::CProxyImpl<Derived, "
     << traits_name << "> {\n";
  os << "    using Base = wl::CProxyImpl<Derived, " << traits_name << ">;\n\n";
  os << "public:\n";

  for (const auto& r : iface.requests) {
    std::string method = snake_to_pascal(r.name);
    os << "    void " << method << "(";
    for (std::size_t i = 0; i < r.args.size(); ++i) {
      if (i > 0)
        os << ", ";
      os << cpp_arg_type(r.args[i]) << " " << r.args[i].name;
    }
    os << ") noexcept {\n";
    os << "        Base::_Marshal(" << traits_name << "::Op::" << method;
    for (const auto& a : r.args)
      os << ", " << a.name;
    if (r.is_destructor)
      os << ");\n        wl_proxy_destroy(Base::Detach());\n    }\n\n";
    else
      os << ");\n    }\n\n";
  }

  for (const auto& e : iface.events)
    emit_crack_event(os, e);

  for (const auto& e : iface.events) {
    std::string handler = "On" + snake_to_pascal(e.name);
    os << "    virtual void " << handler << "(";
    for (std::size_t i = 0; i < e.args.size(); ++i) {
      if (i > 0)
        os << ", ";
      os << cpp_arg_type(e.args[i]) << " /*" << e.args[i].name << "*/";
    }
    os << ") {}\n";
  }

  if (!iface.events.empty()) {
    os << "\n    BEGIN_EVENT_MAP(" << cls_name << ")\n";
    for (const auto& e : iface.events) {
      // Use the raw integer opcode so the EVENT_HANDLER macro's ## token-paste
      // produces a valid C++ identifier (_CrackEvent_<N>).
      os << "        EVENT_HANDLER(" << e.opcode << ", On"
         << snake_to_pascal(e.name) << ")\n";
    }
    os << "    END_EVENT_MAP()\n\n";

    os << "private:\n";
    // Allow the CRTP base to access the private vtable.
    os << "    friend class wl::CProxyImpl<Derived, " << traits_name << ">;\n\n";
    for (const auto& e : iface.events) {
      std::string fn = "_Evt" + snake_to_pascal(e.name);
      os << "    static void " << fn << "(void* data, wl_proxy* /*proxy*/";
      for (const auto& a : e.args)
        os << ", " << cpp_arg_type(a) << " " << a.name;
      os << ") {\n";

      if (e.args.empty()) {
        os << "        void* args[] = {nullptr};\n";
      } else {
        os << "        void* args[] = {";
        for (std::size_t i = 0; i < e.args.size(); ++i) {
          if (i > 0)
            os << ", ";
          os << "&" << e.args[i].name;
        }
        os << "};\n";
      }
      os << "        static_cast<" << cls_name << "*>(data)->ProcessEvent("
         << traits_name << "::Evt::" << snake_to_pascal(e.name) << ", args);\n";
      os << "    }\n";
    }
    // reinterpret_cast is not a constant expression, so we cannot use
    // constexpr here.  The inline keyword makes the definition valid inside
    // the class body for all C++17+ (this project requires C++23).
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
    os << "    inline static const void* s_listener_table_[] = {\n";
    for (const auto& e : iface.events)
      os << "        reinterpret_cast<const void*>(&_Evt"
         << snake_to_pascal(e.name) << "),\n";
    os << "    };\n";
  }

  os << "};\n\n";
}

}  // anonymous namespace

std::string generate_client_cxx_header(const Protocol& proto) {
  // S6: all identifiers validated at parse time; safe to emit directly.
  std::ostringstream os;

  os << "// SPDX-License-Identifier: MIT\n";
  os << "// AUTO-GENERATED by wayland-cxx-scanner 0.1.0 — DO NOT EDIT\n";
  os << "#pragma once\n\n";
  os << "#include <wl/proxy_impl.hpp>\n";
  os << "#include <wl/event_map.hpp>\n\n";
  os << "#include <cstdint>\n";
  os << "#include <string_view>\n\n";

  std::string ns = proto.name + "::client";
  os << "namespace " << ns << " {\n\n";

  for (const auto& iface : proto.interfaces) {
    for (const auto& en : iface.enums)
      emit_enum(os, iface, en);
    emit_traits(os, iface);
    emit_client_class(os, iface);
  }

  os << "}  // namespace " << ns << "\n";
  return os.str();
}

}  // namespace wl::scanner
