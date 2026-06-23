// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
// IVirtualKeyboard — inject keycodes into the seat (the wvkbd-style role):
// upload an xkb keymap over an fd and emit key / modifier events.
//
// Header-only contract only; the virtual-keyboard-v1 backend selected at build
// time implements IVirtualKeyboard. See <wl/ime/backend.hpp>.
#include <cstdint>

namespace wl::ime {

class IVirtualKeyboard {
 public:
  /// Upload the keymap. @p format is a WL_KEYBOARD_KEYMAP_FORMAT_* value and
  /// @p fd refers to a readable keymap of @p size bytes.
  virtual void SetKeymap(int fd, uint32_t size, uint32_t format) = 0;
  virtual void Key(uint32_t time, uint32_t keycode, uint32_t state) = 0;
  virtual void Modifiers(uint32_t depressed,
                         uint32_t latched,
                         uint32_t locked,
                         uint32_t group) = 0;
  virtual ~IVirtualKeyboard() = default;
};

}  // namespace wl::ime
