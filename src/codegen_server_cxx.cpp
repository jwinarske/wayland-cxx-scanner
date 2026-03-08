// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "codegen_server_cxx.hpp"

#include "name_transform.hpp"

#include <cassert>
#include <sstream>

namespace wl::scanner {

using namespace ir;

namespace {

/// Map ArgType to a C++ parameter type for server-side code.
/// M3: every ArgType value is explicitly listed; the default branch is
/// unreachable and asserts in debug builds.
std::string cpp_server_arg_type(const Arg& arg) {
    switch (arg.type) {
        case ArgType::Int:    return "int32_t";
        case ArgType::Uint:   return "uint32_t";
        case ArgType::Fixed:  return "wl_fixed_t";
        case ArgType::String: return "const char*";
        case ArgType::Object: return "wl_resource*";
        case ArgType::NewId:  return "uint32_t";
        case ArgType::Array:  return "wl_array*";
        case ArgType::Fd:     return "int32_t";
        case ArgType::Enum:   return "uint32_t";
    }
    assert(false && "unhandled ArgType in cpp_server_arg_type");
    return "void*";
}

void emit_server_traits(std::ostringstream& os, const Interface& iface) {
    // S6: names validated at parse time.
    std::string traits_name = iface.name + "_server_traits";
    os << "struct " << traits_name << " {\n";
    os << "    static constexpr std::string_view interface_name = \"" << iface.name << "\";\n";
    os << "    static constexpr uint32_t         version        = " << iface.version << ";\n";
    os << "    static const wl_interface& wl_iface() noexcept;\n";

    if (!iface.requests.empty()) {
        os << "    struct Req {\n";
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

void emit_crack_request(std::ostringstream& os, const Message& req) {
    os << "    template <typename T, typename Fn>\n";
    os << "    static void _CrackRequest_" << req.opcode
       << "(T* self, wl_client* client, wl_resource* resource, void** args, Fn fn) {\n";
    os << "        (self->*fn)(client, resource";
    for (std::size_t i = 0; i < req.args.size(); ++i)
        os << ", *reinterpret_cast<" << cpp_server_arg_type(req.args[i]) << "*>(args[" << i
           << "])";
    os << ");\n    }\n\n";
}

void emit_server_class(std::ostringstream& os, const Interface& iface) {
    // S6: names validated at parse time.
    std::string cls_name    = "C" + snake_to_pascal(iface.name) + "Server";
    std::string traits_name = iface.name + "_server_traits";

    emit_server_traits(os, iface);

    os << "template <class Derived>\n";
    os << "class " << cls_name << " : public wl::CResourceImpl<Derived, " << traits_name
       << "> {\n";
    os << "    using Base = wl::CResourceImpl<Derived, " << traits_name << ">;\n\n";
    os << "public:\n";

    for (const auto& e : iface.events) {
        std::string method = "Send" + snake_to_pascal(e.name);
        os << "    void " << method << "(";
        for (std::size_t i = 0; i < e.args.size(); ++i) {
            if (i > 0)
                os << ", ";
            os << cpp_server_arg_type(e.args[i]) << " " << e.args[i].name;
        }
        os << ") noexcept {\n";
        os << "        Base::_PostEvent(" << traits_name << "::Evt::" << snake_to_pascal(e.name);
        for (const auto& a : e.args)
            os << ", " << a.name;
        os << ");\n    }\n\n";
    }

    for (const auto& r : iface.requests)
        emit_crack_request(os, r);

    for (const auto& r : iface.requests) {
        std::string handler = "On" + snake_to_pascal(r.name);
        os << "    virtual void " << handler
           << "(wl_client* /*client*/, wl_resource* /*resource*/";
        for (const auto& a : r.args)
            os << ", " << cpp_server_arg_type(a) << " /*" << a.name << "*/";
        os << ") {}\n";
    }

    if (!iface.requests.empty()) {
        os << "\n    BEGIN_REQUEST_MAP(" << cls_name << ")\n";
        for (const auto& r : iface.requests)
            os << "        REQUEST_HANDLER(" << traits_name << "::Req::"
               << snake_to_pascal(r.name) << ", On" << snake_to_pascal(r.name) << ")\n";
        os << "    END_REQUEST_MAP()\n\n";

        os << "private:\n";
        for (const auto& r : iface.requests) {
            std::string fn = "_Req" + snake_to_pascal(r.name);
            // R5: generated dispatch functions null-check the user_data pointer
            // before dereferencing, guarding against uninitialized resources.
            os << "    static void " << fn
               << "(wl_client* client, wl_resource* resource, void** args) {\n";
            os << "        auto* self = static_cast<" << cls_name
               << "*>(wl_resource_get_user_data(resource));\n";
            os << "        if (!self) return;  // R5: guard against uninitialised resource\n";
            os << "        self->ProcessRequest(" << traits_name << "::Req::"
               << snake_to_pascal(r.name) << ", client, resource, args);\n";
            os << "    }\n";
        }
        os << "    static constexpr void* s_request_vtable_[] = {\n";
        for (const auto& r : iface.requests)
            os << "        reinterpret_cast<void*>(&_Req" << snake_to_pascal(r.name) << "),\n";
        os << "    };\n";
    }

    os << "};\n\n";
}

}  // anonymous namespace

std::string generate_server_cxx_header(const Protocol& proto) {
    // S6: all identifiers validated at parse time; safe to emit directly.
    std::ostringstream os;

    os << "// SPDX-License-Identifier: MIT\n";
    os << "// AUTO-GENERATED by wayland-cxx-scanner 0.1.0 — DO NOT EDIT\n";
    os << "#pragma once\n\n";
    os << "#include <wl/resource_impl.hpp>\n";
    os << "#include <wl/event_map.hpp>\n\n";
    os << "#include <cstdint>\n";
    os << "#include <string_view>\n\n";

    std::string ns = proto.name + "::server";
    os << "namespace " << ns << " {\n\n";

    for (const auto& iface : proto.interfaces)
        emit_server_class(os, iface);

    os << "}  // namespace " << ns << "\n";
    return os.str();
}

}  // namespace wl::scanner
