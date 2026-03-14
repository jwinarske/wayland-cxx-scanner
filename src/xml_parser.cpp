// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "xml_parser.hpp"

#include "ir.hpp"
#include "name_transform.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pugixml.hpp>
#include <ranges>
#include <string>

namespace wl::scanner {

using namespace ir;

namespace {

// ── Identifier validation (S6 / M1) ─────────────────────────────────────────

/// Returns true when @p s is a non-empty, valid C/C++ identifier composed
/// only of [A-Za-z0-9_] with no leading digit.  This prevents code-injection
/// through crafted XML names reaching generated source files.
bool is_valid_identifier(std::string_view s) noexcept {
  if (s.empty())
    return false;
  if (s[0] != '_' && !(s[0] >= 'A' && s[0] <= 'Z') &&
      !(s[0] >= 'a' && s[0] <= 'z'))
    return false;
  return std::ranges::all_of(s, [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
  });
}

void require_identifier(std::string_view s, const char* ctx) {
  if (!is_valid_identifier(s))
    throw ParseError(std::string("invalid identifier '") + std::string(s) +
                     "' on " + ctx);
}

// ── Numeric parsing helpers (S1 — no-throw integer parsing) ─────────────────

/// Parse a non-negative integer, accepting decimal and hex ("0x…").
/// Throws ParseError on overflow or invalid input.
uint32_t parse_uint32(const char* s, const char* ctx) {
  if (!s || *s == '\0')
    throw ParseError(std::string("missing integer value for ") + ctx);
  char* endptr = nullptr;
  unsigned long v = std::strtoul(s, &endptr, 0);
  if (endptr == s || *endptr != '\0')
    throw ParseError(std::string("invalid integer '") + s + "' for " + ctx);
  if (v > UINT32_MAX)
    throw ParseError(std::string("integer overflow '") + s + "' for " + ctx);
  return static_cast<uint32_t>(v);
}

// ── Arg type mapping ─────────────────────────────────────────────────────────

ArgType parse_arg_type(const char* type_str, const char* arg_name) {
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
  throw ParseError(std::string("unknown arg type '") + type_str + "' on arg '" +
                   arg_name + "'");
}

// ── DOM traversal
// ─────────────────────────────────────────────────────────────

Arg parse_arg(const pugi::xml_node node) {
  const char* name = node.attribute("name").value();
  const char* type_str = node.attribute("type").value();
  const char* iface_str = node.attribute("interface").as_string(nullptr);
  const char* enum_str = node.attribute("enum").as_string(nullptr);
  const char* null_ok = node.attribute("allow-null").as_string(nullptr);

  if (*name == '\0')
    throw ParseError("missing 'name' on <arg>");
  if (*type_str == '\0')
    throw ParseError(std::string("missing 'type' on <arg> '") + name + "'");
  require_identifier(name, "<arg> name");

  Arg arg;
  arg.name = name;
  const ArgType raw = parse_arg_type(type_str, name);

  if (enum_str && *enum_str != '\0') {
    arg.type = ArgType::Enum;
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

Message parse_message(const pugi::xml_node node, const uint32_t opcode) {
  const char* name = node.attribute("name").value();
  if (*name == '\0')
    throw ParseError(std::string("missing 'name' on <") + node.name() + ">");
  require_identifier(name, node.name());

  Message msg;
  msg.name = name;
  msg.opcode = opcode;

  if (const char* type_attr = node.attribute("type").as_string(nullptr);
      type_attr && std::strcmp(type_attr, "destructor") == 0)
    msg.is_destructor = true;

  // Parse optional since="N" version attribute.
  if (const char* since_attr = node.attribute("since").as_string(nullptr);
      since_attr && *since_attr != '\0')
    msg.since = since_attr;

  for (pugi::xml_node arg_node : node.children("arg"))
    msg.args.push_back(parse_arg(arg_node));

  return msg;
}

Enum parse_enum(const pugi::xml_node node) {
  const char* name = node.attribute("name").value();
  if (*name == '\0')
    throw ParseError("missing 'name' on <enum>");
  require_identifier(name, "<enum> name");

  Enum en;
  en.name = name;
  en.is_bitfield =
      std::strcmp(node.attribute("bitfield").as_string("false"), "true") == 0;

  for (pugi::xml_node entry : node.children("entry")) {
    const char* ename = entry.attribute("name").value();
    const char* eval = entry.attribute("value").value();
    if (*ename == '\0' || *eval == '\0')
      continue;
    // Entry names must be non-empty and composed only of [A-Za-z0-9_].
    // A leading digit is permitted here (e.g. "90", "180" in
    // wl_output.transform); the C++ codegens prefix such names with '_'
    // to produce a valid identifier.  Reject anything with other characters
    // to guard against injection through crafted XML.
    const bool body_ok =
        std::ranges::all_of(std::string_view(ename), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        });
    if (!body_ok) {
      std::fprintf(stderr,
                   "wayland-cxx-scanner: skipping <entry name=\"%s\"> "
                   "(contains non-identifier characters)\n",
                   ename);
      continue;
    }

    EnumEntry e;
    e.name = ename;
    e.value = parse_uint32(eval, "<entry> value");
    en.entries.push_back(std::move(e));
  }
  return en;
}

Protocol parse_doc(const pugi::xml_document& doc) {
  const pugi::xml_node protocol_node = doc.child("protocol");
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
    iface.version = (ver_str && *ver_str != '\0')
                        ? parse_uint32(ver_str, "<interface> version")
                        : 1u;

    uint32_t req_opcode = 0;
    for (const pugi::xml_node req : iface_node.children("request"))
      iface.requests.push_back(parse_message(req, req_opcode++));

    uint32_t evt_opcode = 0;
    for (const pugi::xml_node evt : iface_node.children("event"))
      iface.events.push_back(parse_message(evt, evt_opcode++));

    for (const pugi::xml_node en : iface_node.children("enum"))
      iface.enums.push_back(parse_enum(en));

    proto.interfaces.push_back(std::move(iface));
  }

  return proto;
}

}  // anonymous namespace

// ── Public API ───────────────────────────────────────────────────────────────

Protocol parse_protocol_from_string(const std::string_view xml) {
  pugi::xml_document doc;
  if (const pugi::xml_parse_result result =
          doc.load_buffer(xml.data(), xml.size());
      !result)
    throw ParseError(std::string("XML parse error: ") + result.description());
  return parse_doc(doc);
}

Protocol parse_protocol(const std::filesystem::path& path) {
  // Open explicitly to generate std::system_error on access failure (R2, R3).
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs)
    throw std::system_error(errno, std::generic_category(),
                            "cannot open " + path.string());

  // Validate file size before allocation (R3: guard against negative tellg).
  const auto raw_size = ifs.tellg();
  if (raw_size < 0)
    throw std::system_error(errno, std::generic_category(),
                            "cannot determine size of " + path.string());
  const auto file_size = static_cast<std::size_t>(raw_size);

  ifs.seekg(0);
  std::string buf(file_size, '\0');
  ifs.read(buf.data(), static_cast<std::streamsize>(file_size));

  // R2: verify the full file was read (guards against EINTR / short reads).
  if (static_cast<std::size_t>(ifs.gcount()) != file_size)
    throw std::system_error(errno, std::generic_category(),
                            "short read on " + path.string());

  return parse_protocol_from_string(buf);
}

}  // namespace wl::scanner
