// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
// text-input-v3 backend: implements the ITextInputReceiver facade on top of
// zwp_text_input_v3 / zwp_text_input_manager_v3.
//
// v3 differs from v1 in the activation order: the text_input is created per
// seat (get_text_input(seat)), the compositor sends enter() when a surface is
// focused, and only THEN does the client enable().  This backend records the
// consumer's activate intent and (re)issues enable()+commit() on enter, so the
// same ITextInputReceiver usage drives both v1 and v3.
//
// Include order (mirrors the seat.hpp pattern): the consumer's build generates
// text_input_v3_client.hpp with --emit-interface-tables and this header
// includes it by that fixed name.
#include <text_input_v3_client.hpp>  // namespace text_input_unstable_v3::client

#include <wl/client_helpers.hpp>
#include <wl/ime/text_input_receiver.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

#include <algorithm>  // std::min
#include <cstdint>
#include <string>
#include <string_view>

namespace wl::ime {

/// Coalesce a nullable zwp_text_input_v3 preedit string to a view.
///
/// A nil @p text is the protocol's "no preedit" signal: it clears any active
/// preedit (e.g. backspace over a one-character composing run). It must be
/// forwarded to the consumer as an empty string so the consumer can end its
/// composing run — dropping the event leaves stale preedit on screen.
inline std::string_view CoalescePreedit(const char* text) noexcept {
  return text ? std::string_view{text} : std::string_view{};
}

/// Concrete ITextInputReceiver backed by zwp_text_input_v3.
class TextInputV3Backend : public ITextInputReceiver {
 public:
  TextInputV3Backend() noexcept = default;
  TextInputV3Backend(const TextInputV3Backend&) = delete;
  TextInputV3Backend& operator=(const TextInputV3Backend&) = delete;
  TextInputV3Backend(TextInputV3Backend&&) = delete;
  TextInputV3Backend& operator=(TextInputV3Backend&&) = delete;
  ~TextInputV3Backend() override { Release(); }

  void Record(uint32_t name, uint32_t ver) noexcept {
    name_ = name;
    ver_adv_ = ver;
  }

  /// Bind the manager and create the per-seat text_input.  No-op (returns true)
  /// when the manager global was never advertised.  @p seat is required: v3
  /// creates the text_input from it.
  [[nodiscard]] bool Bind(wl::CRegistry& registry,
                          TextInputListener* listener,
                          wl_proxy* seat) noexcept {
    if (!name_)
      return true;
    using namespace text_input_unstable_v3::client;
    listener_ = listener;
    // The manager has no events; bind it without a listener (Attach).
    wl_proxy* mgr_raw = registry.Bind<zwp_text_input_manager_v3_traits>(
        name_, std::min(ver_adv_, zwp_text_input_manager_v3_traits::version));
    if (!mgr_raw)
      return false;
    mgr_.Attach(mgr_raw);
    if (!wl::SetupHandler(
            ti_,
            wl::construct<zwp_text_input_v3_traits,
                          zwp_text_input_manager_v3_traits::Op::GetTextInput>(
                *mgr_.Get(), seat)))
      return false;
    ti_.Get()->backend_ = this;
    return true;
  }

  /// v3 does not need the surface (the text_input is bound to the seat);
  /// accepted for parity with the v1 backend.
  void SetSurface(wl_proxy* /*surface*/) noexcept {}

  void Release() noexcept {
    ti_.Reset();
    mgr_.Reset();
  }

  // ── ITextInputReceiver
  // ──────────────────────────────────────────────────────
  void Activate() override {
    want_active_ = true;
    if (entered_)
      EnableAndCommit();
  }
  void Deactivate() override {
    want_active_ = false;
    if (!ti_.IsNull()) {
      ti_.Get()->Disable();
      ti_.Get()->Commit();
      ++serial_;
    }
  }
  void SetSurroundingText(std::string_view utf8,
                          uint32_t cursor,
                          uint32_t anchor) override {
    if (ti_.IsNull())
      return;
    text_buf_.assign(utf8);
    ti_.Get()->SetSurroundingText(text_buf_.c_str(),
                                  static_cast<int32_t>(cursor),
                                  static_cast<int32_t>(anchor));
  }
  void SetContentType(ContentHint hint, ContentPurpose purpose) override {
    // The facade enums mirror v3's content_hint / content_purpose exactly.
    if (!ti_.IsNull())
      ti_.Get()->SetContentType(static_cast<uint32_t>(hint),
                                static_cast<uint32_t>(purpose));
  }
  void SetCursorRectangle(int32_t x,
                          int32_t y,
                          int32_t width,
                          int32_t height) override {
    if (!ti_.IsNull())
      ti_.Get()->SetCursorRectangle(x, y, width, height);
  }
  void Reset() override {}  // v3 has no reset request; enable() resets state
  void Commit() override {
    if (!ti_.IsNull()) {
      ti_.Get()->Commit();
      ++serial_;
    }
  }
  void ShowPanel() override {}  // v3: compositor-managed (since-2 request)
  void HidePanel() override {}

 private:
  struct Handler
      : public text_input_unstable_v3::client::CZwpTextInputV3<Handler> {
    TextInputV3Backend* backend_ = nullptr;

    void OnEnter(wl_proxy* /*surface*/) override {
      backend_->entered_ = true;
      // The field is focused now; (re)enable if the consumer asked to activate.
      if (backend_->want_active_)
        backend_->EnableAndCommit();
      if (backend_->listener_)
        backend_->listener_->OnEnter();
    }
    void OnLeave(wl_proxy* /*surface*/) override {
      backend_->entered_ = false;
      if (backend_->listener_)
        backend_->listener_->OnLeave();
    }
    void OnPreeditString(const char* text,
                         int32_t cursor_begin,
                         int32_t cursor_end) override {
      // A nil text clears the preedit; forward it as empty (see
      // CoalescePreedit) rather than dropping it, so stale preedit does not
      // linger on screen.
      if (backend_->listener_)
        backend_->listener_->OnPreeditString(CoalescePreedit(text),
                                             cursor_begin, cursor_end);
    }
    void OnCommitString(const char* text) override {
      if (backend_->listener_ && text)
        backend_->listener_->OnCommitString(text);
    }
    void OnDeleteSurroundingText(uint32_t before_length,
                                 uint32_t after_length) override {
      if (backend_->listener_)
        backend_->listener_->OnDeleteSurroundingText(before_length,
                                                     after_length);
    }
    void OnDone(uint32_t serial) override { backend_->serial_ = serial; }
  };

  struct Manager
      : public text_input_unstable_v3::client::CZwpTextInputManagerV3<Manager> {
  };

  void EnableAndCommit() noexcept {
    if (ti_.IsNull())
      return;
    ti_.Get()->Enable();
    ti_.Get()->Commit();
    ++serial_;
  }

  // Declaration order = reverse destruction order: ti_ destroyed before mgr_.
  wl::WlPtr<Manager> mgr_;
  wl::WlPtr<Handler> ti_;

  TextInputListener* listener_ = nullptr;
  std::string text_buf_;
  uint32_t name_ = 0;
  uint32_t ver_adv_ = 0;
  uint32_t serial_ = 0;
  bool want_active_ = false;
  bool entered_ = false;
};

}  // namespace wl::ime
