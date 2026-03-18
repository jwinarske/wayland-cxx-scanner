// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// linux-dmabuf — header-only wl_interface tables and wl_iface() inline
// implementations for the linux-dmabuf-unstable-v1 protocol (version 3).
//
// ── Include order ─────────────────────────────────────────────────────────────
// This header must be included AFTER the generated linux_dmabuf_client.hpp:
//
//   #include "linux_dmabuf_client.hpp"  // defines CZwpLinuxDmabufV1, …
//   #include <wl/linux_dmabuf.hpp>      // tables + wl_iface() impls
//
// ── Provided utilities ────────────────────────────────────────────────────────
//
// Interface tables (namespace wl::dmabuf, version 3):
//   Inline wl_interface objects for zwp_linux_dmabuf_v1 (v3) and
//   zwp_linux_buffer_params_v1 (v3) plus the supporting wl_message arrays.
//   These replace the ~60-line boilerplate block that every linux-dmabuf
//   example previously reproduced verbatim.
//
// wl_iface() implementations (namespace linux_dmabuf_unstable_v1::client):
//   Inline out-of-line definitions of the pure-virtual wl_iface() methods
//   declared in zwp_linux_dmabuf_v1_traits and
//   zwp_linux_buffer_params_v1_traits (linux_dmabuf_client.hpp).
//   Including this header replaces the manual definitions that every example
//   would otherwise duplicate in its .cpp file.
#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <wayland-client-protocol.h>
}

#include <iterator>  // std::data

// ══════════════════════════════════════════════════════════════════════════════
// linux-dmabuf wl_interface definitions (version 3)
//
// There are no pre-built system symbols for linux-dmabuf interfaces (unlike
// core Wayland).  We reproduce the exact same tables that the C
// wayland-scanner generates from linux-dmabuf-unstable-v1.xml so that
// libwayland can type-check and dispatch correctly.
//
// Covered interfaces and versions:
//   zwp_linux_dmabuf_v1         — requests: destroy(v1), create_params(v1)
//                               — events:   format(v1), modifier(v3)
//   zwp_linux_buffer_params_v1  — requests: destroy(v1), add(v1),
//                                           create(v1), create_immed(v2)
//                               — events:   created(v1), failed(v1)
//
// The v4+ interfaces (zwp_linux_dmabuf_feedback_v1 and related requests)
// are deliberately excluded; bind at version 3 to stay within this subset.
//
// All variables are `inline` so each definition is a single instance across
// all translation units that include this header (ODR-safe, C++17 §9.2.6).
// ══════════════════════════════════════════════════════════════════════════════

namespace wl::dmabuf {

// ── Forward declarations ──────────────────────────────────────────────────────
// Needed so the types[] array can reference both interface objects before
// either definition appears.

extern const wl_interface dmabuf_iface;
extern const wl_interface params_iface;

// ── Shared pointer array ──────────────────────────────────────────────────────
// Mirrors linux_dmabuf_unstable_v1_types[] from the C wayland-scanner output.
//
// • avoid-non-const-global-variables: element type must be non-const pointer
//   (const wl_interface*) because wl_message::types is const wl_interface**;
//   adding const to the pointer elements would break the implicit conversion.
// • interfaces-global-init: initializers are object addresses (link-time
//   constants), safe regardless of translation-unit definition order.
// • avoid-c-arrays: mandated by the Wayland C API.
//
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
inline const wl_interface* types[] = {
    nullptr,              // [0]  scalar / no-type slots
    nullptr,              // [1]
    nullptr,              // [2]
    nullptr,              // [3]
    &params_iface,        // [4]  create_params → new_id
    &wl_buffer_interface, // [5]  create_immed → new_id wl_buffer;
                          //      created event → new_id wl_buffer
};

// kScalars points at the null-filled head of types[]; used by messages
// whose non-fd arguments are all scalars.  &types[0] is a constant
// expression so kScalars can be constexpr.
inline constexpr const wl_interface** kScalars = &types[0];

// ── zwp_linux_dmabuf_v1 message tables ───────────────────────────────────────

// clang-format off
inline constexpr wl_message dmabuf_requests[] = {
    {"destroy",       "",  nullptr   },  // opcode 0, v1, destructor
    {"create_params", "n", &types[4] },  // opcode 1, v1
};
inline constexpr wl_message dmabuf_events[] = {
    {"format",   "u",    kScalars},  // opcode 0, v1
    {"modifier", "3uuu", kScalars},  // opcode 1, v3
};

// ── zwp_linux_buffer_params_v1 message tables ─────────────────────────────────

inline constexpr wl_message params_requests[] = {
    {"destroy",      "",       nullptr   },  // opcode 0, v1, destructor
    {"add",          "huuuuu", kScalars  },  // opcode 1, v1 (fd+5×uint)
    {"create",       "iiuu",   kScalars  },  // opcode 2, v1
    {"create_immed", "2niiuu", &types[5] },  // opcode 3, v2
};
inline constexpr wl_message params_events[] = {
    {"created", "n", &types[5]},  // opcode 0, v1
    {"failed",  "",  nullptr  },  // opcode 1, v1
};

// ── Interface object definitions ──────────────────────────────────────────────

inline const wl_interface dmabuf_iface = {
    "zwp_linux_dmabuf_v1", 3,
    2, std::data(dmabuf_requests), 2, std::data(dmabuf_events)};
inline const wl_interface params_iface = {
    "zwp_linux_buffer_params_v1", 3,
    4, std::data(params_requests), 2, std::data(params_events)};
// clang-format on

// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

}  // namespace wl::dmabuf

// ══════════════════════════════════════════════════════════════════════════════
// linux_dmabuf_unstable_v1::client traits — wl_iface() inline implementations
//
// Provide the out-of-line definitions of the pure-virtual wl_iface() methods
// declared by the generated linux_dmabuf_client.hpp.  That header must be
// included before this one so these definitions see the complete traits types.
// ══════════════════════════════════════════════════════════════════════════════

namespace linux_dmabuf_unstable_v1::client {

inline const wl_interface& zwp_linux_dmabuf_v1_traits::wl_iface() noexcept {
  return wl::dmabuf::dmabuf_iface;
}
inline const wl_interface&
zwp_linux_buffer_params_v1_traits::wl_iface() noexcept {
  return wl::dmabuf::params_iface;
}

}  // namespace linux_dmabuf_unstable_v1::client
