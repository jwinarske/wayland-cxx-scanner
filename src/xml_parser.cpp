// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "xml_parser.hpp"

#include "ir.hpp"

#include <expat.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace wl::scanner {

using namespace ir;

namespace {

/// Expat SAX parser state machine.
struct ParserState {
    Protocol              proto;
    std::string           error;
    bool                  in_protocol{false};
    bool                  in_interface{false};
    bool                  in_request{false};
    bool                  in_event{false};
    bool                  in_enum{false};
};

/// Return the value of an XML attribute, or nullptr if absent.
static const char* attr(const char** atts, const char* key) noexcept {
    for (int i = 0; atts[i]; i += 2) {
        if (std::strcmp(atts[i], key) == 0)
            return atts[i + 1];
    }
    return nullptr;
}

/// Map a Wayland arg type string to ArgType.
/// Returns false when the type string is not recognised.
static bool parse_arg_type(const char* type_str, ArgType& out) noexcept {
    struct Pair {
        const char* name;
        ArgType     type;
    };
    static constexpr std::array<Pair, 9> kTable{{
        {"int", ArgType::Int},
        {"uint", ArgType::Uint},
        {"fixed", ArgType::Fixed},
        {"string", ArgType::String},
        {"object", ArgType::Object},
        {"new_id", ArgType::NewId},
        {"array", ArgType::Array},
        {"fd", ArgType::Fd},
        {"enum", ArgType::Uint},  // enum is stored as Uint, flag set separately
    }};
    for (auto& p : kTable) {
        if (std::strcmp(type_str, p.name) == 0) {
            out = p.type;
            return true;
        }
    }
    return false;
}

static void XMLCALL on_start(void* user, const char* el, const char** atts) {
    auto& st = *static_cast<ParserState*>(user);
    if (!st.error.empty())
        return;

    // ── <protocol> ──────────────────────────────────────────────────────────
    if (std::strcmp(el, "protocol") == 0) {
        const char* name = attr(atts, "name");
        if (!name) {
            st.error = "missing 'name' on <protocol>";
            return;
        }
        st.proto.name  = name;
        st.in_protocol = true;
        return;
    }

    if (!st.in_protocol)
        return;

    // ── <interface> ─────────────────────────────────────────────────────────
    if (std::strcmp(el, "interface") == 0) {
        Interface iface;
        const char* name = attr(atts, "name");
        const char* ver  = attr(atts, "version");
        if (!name) {
            st.error = "missing 'name' on <interface>";
            return;
        }
        iface.name    = name;
        iface.version = ver ? static_cast<uint32_t>(std::stoul(ver)) : 1u;
        st.proto.interfaces.push_back(std::move(iface));
        st.in_interface = true;
        return;
    }

    if (!st.in_interface)
        return;

    // ── <request> ───────────────────────────────────────────────────────────
    if (std::strcmp(el, "request") == 0) {
        const char* name = attr(atts, "name");
        if (!name) {
            st.error = "missing 'name' on <request>";
            return;
        }
        Message msg;
        msg.name   = name;
        msg.opcode = static_cast<uint32_t>(st.proto.interfaces.back().requests.size());
        const char* type_attr = attr(atts, "type");
        if (type_attr && std::strcmp(type_attr, "destructor") == 0)
            msg.is_destructor = true;
        st.proto.interfaces.back().requests.push_back(std::move(msg));
        st.in_request = true;
        return;
    }

    // ── <event> ─────────────────────────────────────────────────────────────
    if (std::strcmp(el, "event") == 0) {
        const char* name = attr(atts, "name");
        if (!name) {
            st.error = "missing 'name' on <event>";
            return;
        }
        Message msg;
        msg.name   = name;
        msg.opcode = static_cast<uint32_t>(st.proto.interfaces.back().events.size());
        st.proto.interfaces.back().events.push_back(std::move(msg));
        st.in_event = true;
        return;
    }

    // ── <enum> ──────────────────────────────────────────────────────────────
    if (std::strcmp(el, "enum") == 0) {
        const char* name = attr(atts, "name");
        if (!name) {
            st.error = "missing 'name' on <enum>";
            return;
        }
        Enum e;
        e.name = name;
        const char* bf = attr(atts, "bitfield");
        if (bf && std::strcmp(bf, "true") == 0)
            e.is_bitfield = true;
        st.proto.interfaces.back().enums.push_back(std::move(e));
        st.in_enum = true;
        return;
    }

    // ── <arg> ───────────────────────────────────────────────────────────────
    if (std::strcmp(el, "arg") == 0) {
        if (!st.in_request && !st.in_event)
            return;

        const char* name      = attr(atts, "name");
        const char* type_str  = attr(atts, "type");
        const char* iface_str = attr(atts, "interface");
        const char* enum_str  = attr(atts, "enum");
        const char* null_ok   = attr(atts, "allow-null");

        if (!name) {
            st.error = "missing 'name' on <arg>";
            return;
        }
        if (!type_str) {
            st.error = std::string("missing 'type' on <arg> '") + name + "'";
            return;
        }

        Arg arg;
        arg.name = name;

        ArgType raw_type{};
        if (!parse_arg_type(type_str, raw_type)) {
            st.error = std::string("unknown arg type '") + type_str + "' on arg '" + name + "'";
            return;
        }

        // If an 'enum' attribute is present the semantic type is Enum regardless
        // of whether the wire type is 'uint' or 'int'.
        if (enum_str) {
            arg.type      = ArgType::Enum;
            arg.enum_name = enum_str;
        } else {
            arg.type = raw_type;
        }

        if (iface_str)
            arg.interface_name = iface_str;
        if (null_ok && std::strcmp(null_ok, "true") == 0)
            arg.allow_null = true;

        auto& iface = st.proto.interfaces.back();
        if (st.in_request)
            iface.requests.back().args.push_back(std::move(arg));
        else
            iface.events.back().args.push_back(std::move(arg));
        return;
    }

    // ── <entry> ─────────────────────────────────────────────────────────────
    if (std::strcmp(el, "entry") == 0 && st.in_enum) {
        const char* name  = attr(atts, "name");
        const char* value = attr(atts, "value");
        if (!name || !value)
            return;
        EnumEntry e;
        e.name  = name;
        e.value = static_cast<uint32_t>(std::stoul(value, nullptr, 0));
        st.proto.interfaces.back().enums.back().entries.push_back(std::move(e));
        return;
    }
}

static void XMLCALL on_end(void* user, const char* el) {
    auto& st = *static_cast<ParserState*>(user);
    if (!st.error.empty())
        return;

    if (std::strcmp(el, "protocol") == 0)
        st.in_protocol = false;
    else if (std::strcmp(el, "interface") == 0)
        st.in_interface = false;
    else if (std::strcmp(el, "request") == 0)
        st.in_request = false;
    else if (std::strcmp(el, "event") == 0)
        st.in_event = false;
    else if (std::strcmp(el, "enum") == 0)
        st.in_enum = false;
}

static Protocol run_parser(const char* buf, std::size_t len) {
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p)
        throw ParseError("XML_ParserCreate failed");

    ParserState st;
    XML_SetUserData(p, &st);
    XML_SetElementHandler(p, on_start, on_end);

    bool ok = XML_Parse(p, buf, static_cast<int>(len), /*isFinal=*/1) != XML_STATUS_ERROR;
    std::string xml_err;
    if (!ok) {
        xml_err = std::string("XML parse error at line ") +
                  std::to_string(XML_GetCurrentLineNumber(p)) + ": " +
                  XML_ErrorString(XML_GetErrorCode(p));
    }
    XML_ParserFree(p);

    if (!ok)
        throw ParseError(xml_err);
    if (!st.error.empty())
        throw ParseError(st.error);

    return std::move(st.proto);
}

}  // anonymous namespace

Protocol parse_protocol_from_string(std::string_view xml) {
    return run_parser(xml.data(), xml.size());
}

Protocol parse_protocol(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
        throw std::system_error(errno, std::generic_category(),
                                "cannot open " + path.string());
    auto size = ifs.tellg();
    ifs.seekg(0);
    std::string buf(static_cast<std::size_t>(size), '\0');
    ifs.read(buf.data(), size);
    return run_parser(buf.data(), buf.size());
}

}  // namespace wl::scanner
