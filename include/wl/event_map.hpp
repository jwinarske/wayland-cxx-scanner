// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <cstdint>

namespace wl {

/// Abstract base class for objects that can process Wayland events
/// (client-side) or requests (server-side) by opcode dispatch.
class CEventMap {
 public:
  virtual ~CEventMap() = default;
  virtual bool ProcessEvent(uint32_t opcode, void** args) = 0;
};

}  // namespace wl

// ── Client event map (≈ WTL BEGIN_MSG_MAP / MESSAGE_HANDLER) ─────────────────

#define BEGIN_EVENT_MAP(thisClass)                             \
  using _EventMapClass = thisClass;                            \
  bool ProcessEvent(uint32_t _opcode, void** _args) override { \
    (void)_args;

#define EVENT_HANDLER(opcode, func)                                          \
  if (_opcode == (opcode)) {                                                 \
    _EventMapClass::_CrackEvent_##opcode(static_cast<_EventMapClass*>(this), \
                                         _args, &_EventMapClass::func);      \
    return true;                                                             \
  }

#define CHAIN_EVENT_MAP(baseClass)             \
  if (baseClass::ProcessEvent(_opcode, _args)) \
    return true;

#define CHAIN_EVENT_MAP_MEMBER(member)       \
  if ((member).ProcessEvent(_opcode, _args)) \
    return true;

#define END_EVENT_MAP() \
  return false;         \
  }

// ── Server request map (≈ BEGIN_MSG_MAP server-side) ─────────────────────────

#define BEGIN_REQUEST_MAP(thisClass)                          \
  using _RequestMapClass = thisClass;                         \
  bool ProcessRequest(uint32_t _opcode, wl_client* _client,   \
                      wl_resource* _resource, void** _args) { \
    (void)_args;

#define REQUEST_HANDLER(opcode, func)                                    \
  if (_opcode == (opcode)) {                                             \
    _RequestMapClass::_CrackRequest_##opcode(                            \
        static_cast<_RequestMapClass*>(this), _client, _resource, _args, \
        &_RequestMapClass::func);                                        \
    return true;                                                         \
  }

#define CHAIN_REQUEST_MAP(baseClass)                                 \
  if (baseClass::ProcessRequest(_opcode, _client, _resource, _args)) \
    return true;

#define CHAIN_REQUEST_MAP_MEMBER(member)                           \
  if ((member).ProcessRequest(_opcode, _client, _resource, _args)) \
    return true;

#define END_REQUEST_MAP() \
  return false;           \
  }
