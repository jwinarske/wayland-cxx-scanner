// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include "ir.hpp"

#include <sstream>
#include <string_view>

namespace wl::scanner {

/// Emit, into an already-open `<proto>::<role>` namespace, the inline
/// wl_interface / wl_message / types[] tables for every interface in `proto`,
/// plus inline definitions of each interface's
/// `<iface><traits_suffix>::wl_iface()` returning its table.
///
/// This lets a generated client/server header be fully self-contained for an
/// extension protocol that has no libwayland-provided interface symbol — no
/// hand-written tables and no companion C translation unit.
///
/// @param os            Output stream, positioned inside the role namespace.
/// @param proto         The parsed protocol IR.
/// @param traits_suffix Suffix appended to each interface name to form the
///                      traits type, e.g. "_traits" (client) or
///                      "_server_traits" (server).
void emit_interface_tables(std::ostringstream& os,
                           const ir::Protocol& proto,
                           std::string_view traits_suffix);

}  // namespace wl::scanner
