// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
// ITextInputReceiver — the application end of the text-input protocol family:
// receive committed/preedit text from the compositor's IME and drive the
// per-version request/commit bookkeeping behind a single version-agnostic
// interface. This is the role a downstream shell application needs.
//
// Header-only contract only; a concrete backend (text-input-v1 / -v3) selected
// at build time implements ITextInputReceiver. See <wl/ime/backend.hpp>.
#include <cstdint>
#include <string_view>

namespace wl::ime {

/// Normalized content hints (bitfield).  Values mirror zwp_text_input_v3's
/// content_hint so the v3 backend maps 1:1; the v1 backend translates.
enum class ContentHint : uint32_t {
  kNone = 0x0,
  kCompletion = 0x1,
  kSpellcheck = 0x2,
  kAutoCapitalization = 0x4,
  kLowercase = 0x8,
  kUppercase = 0x10,
  kTitlecase = 0x20,
  kHiddenText = 0x40,
  kSensitiveData = 0x80,
  kLatin = 0x100,
  kMultiline = 0x200,
};

[[nodiscard]] constexpr ContentHint operator|(ContentHint a,
                                              ContentHint b) noexcept {
  return static_cast<ContentHint>(static_cast<uint32_t>(a) |
                                  static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr ContentHint operator&(ContentHint a,
                                              ContentHint b) noexcept {
  return static_cast<ContentHint>(static_cast<uint32_t>(a) &
                                  static_cast<uint32_t>(b));
}

/// Why the surrounding text changed.  Values mirror zwp_text_input_v3's
/// change_cause; v1 has no equivalent request and ignores it.
enum class ChangeCause : uint32_t {
  kInputMethod = 0,  // the input method's own commit changed it
  kOther = 1,        // the application changed it (typing, a caret move, …)
};

/// Normalized content purpose.  Values mirror zwp_text_input_v3's
/// content_purpose.
enum class ContentPurpose : uint32_t {
  kNormal = 0,
  kAlpha = 1,
  kDigits = 2,
  kNumber = 3,
  kPhone = 4,
  kUrl = 5,
  kEmail = 6,
  kName = 7,
  kPassword = 8,
  kPin = 9,
  kDate = 10,
  kTime = 11,
  kDatetime = 12,
  kTerminal = 13,
};

/// Events delivered by the compositor's IME (compositor -> app), normalized
/// across text-input v1 and v3.  The consumer implements this.
struct TextInputListener {
  virtual void OnEnter() = 0;  // text field gained IME focus
  virtual void OnLeave() = 0;
  virtual void OnCommitString(std::string_view utf8) = 0;
  virtual void OnPreeditString(std::string_view utf8,
                               int32_t cursor_begin,
                               int32_t cursor_end) = 0;
  virtual void OnDeleteSurroundingText(uint32_t before_bytes,
                                       uint32_t after_bytes) = 0;
  // v1 only; on v3 ordinary keys arrive via wl_keyboard, so default to no-op.
  virtual void OnKeysym(uint32_t /*time*/,
                        uint32_t /*sym*/,
                        uint32_t /*state*/,
                        uint32_t /*mods*/) {}
  virtual ~TextInputListener() = default;
};

/// Requests the consumer issues (app -> compositor).  The backend owns serial
/// numbering and the per-version commit protocol.
///
/// Every request below only stages a change: nothing reaches the input method
/// until Commit().  Activate() included — on v3 it maps to enable, which resets
/// content type, cursor rectangle and surrounding text, so the state a consumer
/// wants must be sent after it and before the commit that applies them
/// together:
///
///   Activate();
///   SetContentType(...);
///   SetCursorRectangle(...);
///   Commit();
///
/// The same applies after TextInputListener::OnEnter: regaining focus
/// re-enables the text input, which resets that state again, so OnEnter must
/// re-send it and Commit().  A consumer that stages nothing there leaves the
/// input method with defaults — no cursor rectangle for an on-screen keyboard
/// or candidate window to anchor to.
class ITextInputReceiver {
 public:
  virtual void Activate() = 0;    // v1 activate(seat, surface) / v3 enable
  virtual void Deactivate() = 0;  // v1 deactivate / v3 disable
  virtual void SetSurroundingText(std::string_view utf8,
                                  uint32_t cursor,
                                  uint32_t anchor) = 0;
  /// Why the surrounding text last changed.  v3 pairs this with
  /// set_surrounding_text; v1 has no such request and ignores it.
  virtual void SetTextChangeCause(ChangeCause cause) = 0;
  virtual void SetContentType(ContentHint hint, ContentPurpose purpose) = 0;
  virtual void SetCursorRectangle(int32_t x,
                                  int32_t y,
                                  int32_t width,
                                  int32_t height) = 0;
  virtual void Reset() = 0;      // v1 reset / v3 state reset
  virtual void Commit() = 0;     // v1 commit_state(serial) / v3 commit
  virtual void ShowPanel() = 0;  // v1 show_input_panel / v3 no-op
  virtual void HidePanel() = 0;  // v1 hide_input_panel / v3 no-op
  virtual ~ITextInputReceiver() = default;
};

}  // namespace wl::ime
