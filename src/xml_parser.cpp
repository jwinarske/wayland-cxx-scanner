// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "xml_parser.hpp"

#include "ir.hpp"
#include "name_transform.hpp"

#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pugixml.hpp>
#include <string>

namespace wl::scanner {

using namespace ir;

namespace {

// ── Identifier validation (S6 / M1) ─────────────────────────────────────────

/// Returns true when @p s is a non-empty, valid C/C++ identifier composed
/// only of [A-Za-z0-9_] with no leading digit.  This prevents code-injection
/// through crafted XML names reaching generated source files.
static bool is_valid_identifier(std::string_view s) noexcept {
    if (s.empty())
        return false;
    if (s[0] != '_' && (s[0] < 'A' || s[0] > 'z') && !(s[0] >= 'A' && s[0] <= 'Z') &&
        !(s[0] >= 'a' && s[0] <= 'z'))
        return false;
    for (char c : s) {
        if ((c < 'A' || c > 'Z') && (c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '_')
            return false;
    }
    return true;
}

static void require_identifier(std::string_view s, const char* ctx) {
    if (!is_valid_identifier(s))
        throw ParseError(std::string("invalid identifier '") + std::string(s) + "' on " + ctx);
}

// ── Numeric parsing helpers (S1 — no-throw integer parsing) ─────────────────

/// Parse a non-negative integer, accepting decimal and hex ("0x…").
/// Throws ParseError on overflow or invalid input.
static uint32_t parse_uint32(const char* s, const char* ctx) {
    if (!s || *s == '\0')
        throw ParseError(std::string("missing integer value for ") + ctx);
    char* endptr    = nullptr;
    unsigned long v = std::strtoul(s, &endptr, 0);
    if (endptr == s || *endptr != '\0')
        throw ParseError(std::string("invalid integer '") + s + "' for " + ctx);
    if (v > UINT32_MAX)
        throw ParseError(std::string("integer overflow '") + s + "' for " + ctx);
    return static_cast<uint32_t>(v);
}

// ── Arg type mapping ─────────────────────────────────────────────────────────

static ArgType parse_arg_type(const char* type_str, const char* arg_name) {
    struct Pair {
        const char* name;
        ArgType type;
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
        // "enum" wire type: treated as Uint at the wire level; the semantic
        // ArgType::Enum is set later when an "enum" attribute is present.
        {"enum", ArgType::Uint},
    }};
    for (const auto& p : kTable) {
        if (std::strcmp(type_str, p.name) == 0)
            return p.type;
    }
    throw ParseError(std::string("unknown arg type '") + type_str + "' on arg '" + arg_name + "'");
}

// ── DOM traversal ─────────────────────────────────────────────────────────────

static Arg parse_arg(pugi::xml_node node) {
    const char* name      = node.attribute("name").value();
    const char* type_str  = node.attribute("type").value();
    const char* iface_str = node.attribute("interface").as_string(nullptr);
    const char* enum_str  = node.attribute("enum").as_string(nullptr);
    const char* null_ok   = node.attribute("allow-null").as_string(nullptr);

    if (*name == '\0')
        throw ParseError("missing 'name' on <arg>");
    if (*type_str == '\0')
        throw ParseError(std::string("missing 'type' on <arg> '") + name + "'");
    require_identifier(name, "<arg> name");

    Arg arg;
    arg.name    = name;
    ArgType raw = parse_arg_type(type_str, name);

    if (enum_str && *enum_str != '\0') {
        arg.type      = ArgType::Enum;
        arg.enum_name = enum_str;
    } else {
        arg.type = raw;
    }

    if (iface_str && *iface_str != '\0')
        arg.interface_name = iface_str;
    if (null_ok && std::strcmp(null_ok, "true") == 0)
        arg.allow_null = true;

    return arg;
}

static Message parse_message(pugi::xml_node node, uint32_t opcode) {
    const char* name = node.attribute("name").value();
    if (*name == '\0')
        throw ParseError(std::string("missing 'name' on <") + node.name() + ">");
    require_identifier(name, node.name());

    Message msg;
    msg.name   = name;
    msg.opcode = opcode;

    const char* type_attr = node.attribute("type").as_string(nullptr);
    if (type_attr && std::strcmp(type_attr, "destructor") == 0)
        msg.is_destructor = true;

    for (pugi::xml_node arg_node : node.children("arg"))
        msg.args.push_back(parse_arg(arg_node));

    return msg;
}

static Enum parse_enum(pugi::xml_node node) {
    const char* name = node.attribute("name").value();
    if (*name == '\0')
        throw ParseError("missing 'name' on <enum>");
    require_identifier(name, "<enum> name");

    Enum en;
    en.name        = name;
    en.is_bitfield = std::strcmp(node.attribute("bitfield").as_string("false"), "true") == 0;

    for (pugi::xml_node entry : node.children("entry")) {
        const char* ename = entry.attribute("name").value();
        const char* eval  = entry.attribute("value").value();
        if (*ename == '\0' || *eval == '\0')
            continue;
        require_identifier(ename, "<entry> name");

        EnumEntry e;
        e.name  = ename;
        e.value = parse_uint32(eval, "<entry> value");
        en.entries.push_back(std::move(e));
    }
    return en;
}

static Protocol parse_doc(const pugi::xml_document& doc) {
    pugi::xml_node protocol_node = doc.child("protocol");
    if (!protocol_node)
        throw ParseError("missing <protocol> root element");

    const char* proto_name = protocol_node.attribute("name").value();
    if (*proto_name == '\0')
        throw ParseError("missing 'name' on <protocol>");
    require_identifier(proto_name, "<protocol> name");

    Protocol proto;
    proto.name = proto_name;

    for (pugi::xml_node iface_node : protocol_node.children("interface")) {
        const char* iname = iface_node.attribute("name").value();
        if (*iname == '\0')
            throw ParseError("missing 'name' on <interface>");
        require_identifier(iname, "<interface> name");

        Interface iface;
        iface.name = iname;

        const char* ver_str = iface_node.attribute("version").as_string(nullptr);
        iface.version = (ver_str && *ver_str != '\0') ? parse_uint32(ver_str, "<interface> version")
                                                      : 1u;

        uint32_t req_opcode = 0;
        for (pugi::xml_node req : iface_node.children("request"))
            iface.requests.push_back(parse_message(req, req_opcode++));

        uint32_t evt_opcode = 0;
        for (pugi::xml_node evt : iface_node.children("event"))
            iface.events.push_back(parse_message(evt, evt_opcode++));

        for (pugi::xml_node en : iface_node.children("enum"))
            iface.enums.push_back(parse_enum(en));

        proto.interfaces.push_back(std::move(iface));
    }

    return proto;
}

}  // anonymous namespace

// ── Public API ───────────────────────────────────────────────────────────────

Protocol parse_protocol_from_string(std::string_view xml) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    if (!result)
        throw ParseError(std::string("XML parse error: ") + result.description());
    return parse_doc(doc);
}

Protocol parse_protocol(const std::filesystem::path& path) {
    // Open explicitly to generate std::system_error on access failure (R2, R3).
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs)
        throw std::system_error(errno, std::generic_category(), "cannot open " + path.string());

    // Validate file size before allocation (R3: guard against negative tellg).
    const auto raw_size = ifs.tellg();
    if (raw_size < 0)
        throw std::system_error(
            errno, std::generic_category(), "cannot determine size of " + path.string());
    const auto file_size = static_cast<std::size_t>(raw_size);

    ifs.seekg(0);
    std::string buf(file_size, '\0');
    ifs.read(buf.data(), static_cast<std::streamsize>(file_size));

    // R2: verify the full file was read (guards against EINTR / short reads).
    if (static_cast<std::size_t>(ifs.gcount()) != file_size)
        throw std::system_error(errno, std::generic_category(), "short read on " + path.string());

    return parse_protocol_from_string(buf);
}

}  // namespace wl::scanner
