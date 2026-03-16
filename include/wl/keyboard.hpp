// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// keyboard — header-only CRTP wl_keyboard handler with xkbcommon keymap
//             processing, modeled after waypp/src/seat/keyboard.cc.
//
// libxkbcommon is assumed to be universally available on any system that
// runs a Wayland compositor; it is treated as a required dependency.
//
// ── Include order
// ───────────────────────────────────────────────────────────── This header
// must be included AFTER the generated wayland_client.hpp:
//
//   #include "wayland_client.hpp"   // defines CWlKeyboard
//   #include <wl/keyboard.hpp>      // wl::KeyboardHandler<App>
//
// ── App contract ─────────────────────────────────────────────────────────────
// The App class must expose:
//
//   void OnKey(uint32_t key, uint32_t state);
//
// where `key` is the Linux evdev keycode (KEY_ESC, KEY_SPACE, …) and `state`
// is WL_KEYBOARD_KEY_STATE_PRESSED or WL_KEYBOARD_KEY_STATE_RELEASED.
//
// App may additionally provide (detected via SFINAE):
//
//   void OnKeySym(xkb_keysym_t sym, uint32_t key, uint32_t state);
//
// When OnKeySym is present it is called BEFORE OnKey on every key event.
// The `sym` is the XKB keysym translated from the current xkb_state
// (WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1); for non-XKB keymaps sym is
// XKB_KEY_NoSymbol and only OnKey is called.
#pragma once

#include <wl/wl_ptr.hpp>

extern "C" {
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
}

#include <cstdint>

// ══════════════════════════════════════════════════════════════════════════════
// wl::KeyboardHandler<App>
//
// CRTP keyboard handler that wraps wl_keyboard and performs full xkbcommon
// keymap processing, modeled on waypp/src/seat/keyboard.cc.
//
// Key design points (all matching waypp's approach):
//   • OnKeymap: mmaps the compositor-provided keymap fd (MAP_PRIVATE for
//     wl_keyboard v7+), compiles it via xkb_keymap_new_from_string, and
//     builds a fresh xkb_state.  The fd is always closed after processing,
//     as required by the Wayland protocol (fd ownership is transferred).
//   • OnKey: translates the Linux evdev scancode to an XKB scancode
//     (evdev_code + 8), looks up the keysym in the current xkb_state,
//     calls app_->OnKeySym(sym, key, state) if the method exists, then
//     always calls app_->OnKey(key, state).
//   • OnModifiers: updates the xkb_state modifier mask so subsequent
//     xkb_state_key_get_one_sym calls reflect the current modifier state.
//   • OnEnter / OnLeave / OnRepeatInfo: no-ops in this implementation.
//
// All xkb objects are reference-counted by libxkbcommon and released in the
// destructor; the handler is safe to destroy at any time.
// ══════════════════════════════════════════════════════════════════════════════

namespace wl {

template <typename App>
class KeyboardHandler
    : public wayland::client::CWlKeyboard<KeyboardHandler<App>> {
 public:
  /// Back-pointer set by SeatManager immediately after SetupHandler() returns.
  App* app_ = nullptr;

  KeyboardHandler() noexcept
      // xkb_context_new() rarely returns nullptr (OOM), but if it does all
      // OnKeymap / OnKey paths guard on xkb_context_ / xkb_state_ being
      // non-null so the handler degrades gracefully (fd is still closed).
      : xkb_context_(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {}

  ~KeyboardHandler() noexcept {
    if (xkb_state_)
      xkb_state_unref(xkb_state_);
    if (xkb_keymap_)
      xkb_keymap_unref(xkb_keymap_);
    if (xkb_context_)
      xkb_context_unref(xkb_context_);
  }

  // Non-copyable, non-movable (owns xkb_* resources).
  KeyboardHandler(const KeyboardHandler&) = delete;
  KeyboardHandler& operator=(const KeyboardHandler&) = delete;
  KeyboardHandler(KeyboardHandler&&) = delete;
  KeyboardHandler& operator=(KeyboardHandler&&) = delete;

  // ── wl_keyboard events ────────────────────────────────────────────────────

  /// Receives the compositor's keymap.  Compiles it via xkbcommon and builds
  /// a fresh xkb_state.  Closes the fd in all code paths (protocol transfers
  /// ownership of the fd to the client).
  void OnKeymap(uint32_t format, int32_t fd, uint32_t size) override {
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && xkb_context_) {
      // wl_keyboard v7+ requires MAP_PRIVATE; older compositors use MAP_SHARED.
      const int mflags = (wl_proxy_get_version(this->GetProxy()) >= 7)
                             ? MAP_PRIVATE
                             : MAP_SHARED;
      auto* keymap_str = static_cast<char*>(mmap(
          nullptr, static_cast<std::size_t>(size), PROT_READ, mflags, fd, 0));
      if (keymap_str != MAP_FAILED) {
        xkb_keymap* new_km = xkb_keymap_new_from_string(
            xkb_context_, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(keymap_str, static_cast<std::size_t>(size));
        if (new_km) {
          xkb_state* new_state = xkb_state_new(new_km);
          if (new_state) {
            // Only swap in the new keymap once we have a valid state for it.
            xkb_keymap_unref(xkb_keymap_);
            xkb_keymap_ = new_km;
            xkb_state_unref(xkb_state_);
            xkb_state_ = new_state;
          } else {
            // State creation failed; keep old keymap+state and discard new_km.
            xkb_keymap_unref(new_km);
          }
        }
      }
    }
    close(fd);
  }

  void OnEnter(uint32_t /*serial*/,
               wl_proxy* /*surface*/,
               wl_array* /*keys*/) override {}

  void OnLeave(uint32_t /*serial*/, wl_proxy* /*surface*/) override {}

  /// Translates the evdev scancode to an XKB keysym and dispatches to App.
  /// Calls OnKeySym(sym, key, state) if available, then OnKey(key, state).
  void OnKey(uint32_t /*serial*/,
             uint32_t /*time*/,
             uint32_t key,
             uint32_t state) override {
    if (!app_)
      return;
    if (xkb_state_) {
      // Translate Linux evdev scancode → XKB scancode (evdev offset is 8).
      const xkb_keycode_t xkb_code = static_cast<xkb_keycode_t>(key) + 8u;
      const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state_, xkb_code);
      CallOnKeySym(sym, key, state);
    }
    app_->OnKey(key, state);
  }

  /// Updates the xkb modifier state so subsequent keysym lookups are correct.
  void OnModifiers(uint32_t /*serial*/,
                   uint32_t mods_depressed,
                   uint32_t mods_latched,
                   uint32_t mods_locked,
                   uint32_t group) override {
    if (xkb_state_)
      xkb_state_update_mask(xkb_state_, mods_depressed, mods_latched,
                            mods_locked, 0, 0, group);
  }

  void OnRepeatInfo(int32_t /*rate*/, int32_t /*delay*/) override {}

 private:
  xkb_context* xkb_context_ = nullptr;
  xkb_keymap* xkb_keymap_ = nullptr;
  xkb_state* xkb_state_ = nullptr;

  // SFINAE: call app_->OnKeySym if the method exists.
  template <typename A = App>
  auto CallOnKeySym(xkb_keysym_t sym, uint32_t key, uint32_t state)
      -> decltype(std::declval<A&>().OnKeySym(sym, key, state), void()) {
    app_->OnKeySym(sym, key, state);
  }
  // Fallback: OnKeySym not present — do nothing.
  void CallOnKeySym(...) noexcept {}
};

}  // namespace wl
