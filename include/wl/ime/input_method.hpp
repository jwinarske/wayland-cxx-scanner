// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
// IInputMethod — the IME end: be the input method the compositor routes focused
// text fields to, and commit text back to them. For a consumer that implements
// an on-screen keyboard. Normalizes weston's per-activation
// input_method_context against zwp_input_method_v2's persistent object.
//
// Header-only contract only; a concrete backend (input-method-v1 / -v2)
// selected at build time implements IInputMethod. See <wl/ime/backend.hpp>.
#include <cstdint>
#include <string_view>

#include <wl/ime/text_input_receiver.hpp>  // ContentHint, ContentPurpose

namespace wl::ime {

/// Events delivered to the IME (compositor -> IME), normalized across
/// input-method v1 and v2.  The consumer implements this.
struct InputMethodListener {
  virtual void OnActivate() = 0;  // a text field became active
  virtual void OnDeactivate() = 0;
  virtual void OnSurroundingText(std::string_view utf8,
                                 uint32_t cursor,
                                 uint32_t anchor) = 0;
  virtual void OnContentType(ContentHint hint, ContentPurpose purpose) = 0;
  virtual void OnDone() = 0;
  virtual ~InputMethodListener() = default;
};

/// Requests the IME issues (IME -> compositor).  The backend owns serial
/// numbering and the per-version commit protocol.
class IInputMethod {
 public:
  virtual void CommitString(std::string_view utf8) = 0;
  virtual void SetPreeditString(std::string_view utf8,
                                int32_t cursor_begin,
                                int32_t cursor_end) = 0;
  virtual void DeleteSurroundingText(uint32_t before, uint32_t after) = 0;
  virtual void Commit() = 0;  // backend supplies/advances the serial
  virtual ~IInputMethod() = default;
};

}  // namespace wl::ime
