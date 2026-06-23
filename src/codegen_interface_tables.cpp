// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#include "codegen_interface_tables.hpp"

#include <set>
#include <string>
#include <vector>

namespace wl::scanner {

using namespace ir;

namespace {

uint32_t msg_since(const Message& m) {
  return m.since.empty() ? 1u : static_cast<uint32_t>(std::stoul(m.since));
}

/// One slot in the protocol-wide types[] array.  Each wire-argument position of
/// a message maps to exactly one slot; only object/new_id positions carry an
/// interface pointer (libwayland ignores the slot for scalar positions).
struct Slot {
  enum Kind { Null, Internal, External } kind = Null;
  std::string iface;  // interface name for Internal / External
};

/// Expand a message's arguments into their wire-signature type slots.  A normal
/// argument is one slot; a new_id with no interface expands to three (the
/// implicit interface-name string + version uint + the id), matching the wire
/// encoding wl_registry.bind uses.
std::vector<Slot> slots_for(const Message& m,
                            const std::set<std::string>& internal) {
  std::vector<Slot> slots;
  for (const auto& a : m.args) {
    switch (a.type) {
      case ArgType::Object:
        if (a.interface_name.empty())
          slots.push_back({Slot::Null, {}});
        else
          slots.push_back({internal.count(a.interface_name) ? Slot::Internal
                                                            : Slot::External,
                           a.interface_name});
        break;
      case ArgType::NewId:
        if (a.interface_name.empty()) {
          // Implicit "su" (interface name + version) then the new id.
          slots.push_back({Slot::Null, {}});
          slots.push_back({Slot::Null, {}});
          slots.push_back({Slot::Null, {}});
        } else {
          slots.push_back({internal.count(a.interface_name) ? Slot::Internal
                                                            : Slot::External,
                           a.interface_name});
        }
        break;
      default:
        slots.push_back({Slot::Null, {}});
        break;
    }
  }
  return slots;
}

/// Build the wl_message signature string: an optional since-version digit
/// prefix followed by one character per wire-argument position.
std::string wire_signature(const Message& m) {
  std::string sig;
  if (const uint32_t since = msg_since(m); since > 1)
    sig += std::to_string(since);
  for (const auto& a : m.args) {
    const bool nullable = a.allow_null;
    switch (a.type) {
      case ArgType::Int:
        sig += 'i';
        break;
      case ArgType::Uint:
      case ArgType::Enum:
        sig += 'u';
        break;
      case ArgType::Fixed:
        sig += 'f';
        break;
      case ArgType::String:
        if (nullable)
          sig += '?';
        sig += 's';
        break;
      case ArgType::Object:
        if (nullable)
          sig += '?';
        sig += 'o';
        break;
      case ArgType::Array:
        if (nullable)
          sig += '?';
        sig += 'a';
        break;
      case ArgType::Fd:
        sig += 'h';
        break;
      case ArgType::NewId:
        if (a.interface_name.empty())
          sig += "su";
        sig += 'n';
        break;
    }
  }
  return sig;
}

/// Slot offset/length for one message within the protocol-wide types[] array.
struct MsgTable {
  std::string signature;
  std::size_t offset = 0;
  std::size_t count = 0;
};

void emit_slot(std::ostringstream& os, const Slot& s) {
  switch (s.kind) {
    case Slot::Internal:
      os << "    &" << s.iface << "_iface,\n";
      break;
    case Slot::External:
      os << "    &" << s.iface << "_interface,\n";
      break;
    case Slot::Null:
      os << "    nullptr,\n";
      break;
  }
}

}  // namespace

void emit_interface_tables(std::ostringstream& os,
                           const Protocol& proto,
                           std::string_view traits_suffix) {
  std::set<std::string> internal;
  for (const auto& iface : proto.interfaces)
    internal.insert(iface.name);

  // Collect external interfaces referenced by object/new_id arguments.
  std::set<std::string> external;
  for (const auto& iface : proto.interfaces)
    for (const auto* list : {&iface.requests, &iface.events})
      for (const auto& m : *list)
        for (const auto& a : m.args)
          if ((a.type == ArgType::Object || a.type == ArgType::NewId) &&
              !a.interface_name.empty() && !internal.count(a.interface_name))
            external.insert(a.interface_name);

  // Flatten every message's slots into the protocol-wide types[] array,
  // recording each message's offset and length.  Order: per interface,
  // requests then events (matches the opcode order libwayland dispatches on).
  std::vector<Slot> all_slots;
  std::vector<std::vector<MsgTable>> reqs(proto.interfaces.size());
  std::vector<std::vector<MsgTable>> evts(proto.interfaces.size());

  for (std::size_t i = 0; i < proto.interfaces.size(); ++i) {
    const auto& iface = proto.interfaces[i];
    auto record = [&](const Message& m) {
      MsgTable t;
      t.signature = wire_signature(m);
      t.offset = all_slots.size();
      auto slots = slots_for(m, internal);
      t.count = slots.size();
      for (auto& s : slots)
        all_slots.push_back(std::move(s));
      return t;
    };
    for (const auto& r : iface.requests)
      reqs[i].push_back(record(r));
    for (const auto& e : iface.events)
      evts[i].push_back(record(e));
  }

  os << "namespace detail {\n\n";

  // Forward declarations so types[] can take addresses before definitions.
  if (!external.empty()) {
    os << "extern \"C\" {\n";
    for (const auto& name : external)
      os << "extern const wl_interface " << name << "_interface;\n";
    os << "}\n\n";
  }
  for (const auto& iface : proto.interfaces)
    os << "extern const wl_interface " << iface.name << "_iface;\n";
  os << "\n";

  // clang-format off
  os << "// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,"
        "cppcoreguidelines-avoid-non-const-global-variables,"
        "cppcoreguidelines-interfaces-global-init)\n";
  // clang-format on

  // Shared types[] array.
  os << "inline const wl_interface* iface_types[] = {\n";
  if (all_slots.empty())
    os << "    nullptr,\n";
  else
    for (const auto& s : all_slots)
      emit_slot(os, s);
  os << "};\n\n";

  // Per-interface message tables.
  auto emit_msg_table = [&](std::string_view iface_name, std::string_view kind,
                            const std::vector<Message>& msgs,
                            const std::vector<MsgTable>& tables) {
    if (msgs.empty())
      return;
    os << "inline constexpr wl_message " << iface_name << "_" << kind
       << "[] = {\n";
    for (std::size_t j = 0; j < msgs.size(); ++j) {
      os << "    {\"" << msgs[j].name << "\", \"" << tables[j].signature
         << "\", ";
      if (tables[j].count == 0)
        os << "nullptr";
      else
        os << "iface_types + " << tables[j].offset;
      os << "},\n";
    }
    os << "};\n\n";
  };

  for (std::size_t i = 0; i < proto.interfaces.size(); ++i) {
    const auto& iface = proto.interfaces[i];
    emit_msg_table(iface.name, "requests", iface.requests, reqs[i]);
    emit_msg_table(iface.name, "events", iface.events, evts[i]);
  }

  // wl_interface definitions.
  for (const auto& iface : proto.interfaces) {
    os << "inline const wl_interface " << iface.name << "_iface = {\n";
    os << "    \"" << iface.name << "\", " << iface.version << ",\n";
    if (iface.requests.empty())
      os << "    0, nullptr,\n";
    else
      os << "    " << iface.requests.size() << ", " << iface.name
         << "_requests,\n";
    if (iface.events.empty())
      os << "    0, nullptr};\n\n";
    else
      os << "    " << iface.events.size() << ", " << iface.name
         << "_events};\n\n";
  }

  os << "// NOLINTEND(cppcoreguidelines-avoid-c-arrays,"
        "cppcoreguidelines-avoid-non-const-global-variables,"
        "cppcoreguidelines-interfaces-global-init)\n\n";

  os << "}  // namespace detail\n\n";

  // wl_iface() definitions binding each traits type to its table.
  for (const auto& iface : proto.interfaces) {
    os << "inline const wl_interface& " << iface.name << traits_suffix
       << "::wl_iface() noexcept {\n";
    os << "  return detail::" << iface.name << "_iface;\n";
    os << "}\n";
  }
  os << "\n";
}

}  // namespace wl::scanner
