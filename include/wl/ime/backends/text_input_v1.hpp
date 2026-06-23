// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
// text-input-v1 backend: implements the ITextInputReceiver facade on top of
// zwp_text_input_v1 / zwp_text_input_manager_v1.
//
// Include order (mirrors the seat.hpp pattern): the consumer's build generates
// text_input_v1_client.hpp from text-input-unstable-v1.xml with
// --emit-interface-tables, and this header includes it by that fixed name.
#include <text_input_v1_client.hpp>  // namespace text_input_unstable_v1::client

#include <wl/client_helpers.hpp>
#include <wl/ime/text_input_receiver.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>

#include <algorithm>  // std::min
#include <cstdint>
#include <string>
#include <string_view>

namespace wl::ime {

/// Concrete ITextInputReceiver backed by zwp_text_input_v1.
///
/// Setup (backend-specific, outside the facade contract):
///   1. Record(name, ver)  -- from the registry OnGlobal callback for
///                            zwp_text_input_manager_v1.
///   2. Bind(registry, listener, seat) -- bind the manager, create the
///                            text_input, and install the event handler.  v1's
///                            activate() needs the seat, so it is supplied
///                            here.
///   3. SetSurface(surface) -- the surface that owns the text field; update it
///                            as focus moves.  Required before Activate().
/// The ITextInputReceiver methods then drive the protocol; the backend owns the
/// commit serial and the per-event bookkeeping.
class TextInputV1Backend : public ITextInputReceiver {
 public:
  TextInputV1Backend() noexcept = default;
  TextInputV1Backend(const TextInputV1Backend&) = delete;
  TextInputV1Backend& operator=(const TextInputV1Backend&) = delete;
  TextInputV1Backend(TextInputV1Backend&&) = delete;
  TextInputV1Backend& operator=(TextInputV1Backend&&) = delete;
  ~TextInputV1Backend() override { Release(); }

  void Record(uint32_t name, uint32_t ver) noexcept {
    name_ = name;
    ver_adv_ = ver;
  }

  /// Bind the manager and create the text_input.  No-op (returns true) when the
  /// manager global was never advertised.  @p seat is required for activate().
  [[nodiscard]] bool Bind(wl::CRegistry& registry,
                          TextInputListener* listener,
                          wl_proxy* seat) noexcept {
    if (!name_)
      return true;
    using namespace text_input_unstable_v1::client;
    listener_ = listener;
    seat_ = seat;
    // The manager has no events, so bind it without a listener (Attach), then
    // use it as a factory for the text_input (which does have events).
    wl_proxy* mgr_raw = registry.Bind<zwp_text_input_manager_v1_traits>(
        name_, std::min(ver_adv_, zwp_text_input_manager_v1_traits::version));
    if (!mgr_raw)
      return false;
    mgr_.Attach(mgr_raw);
    if (!wl::SetupHandler(
            ti_, wl::construct<
                     zwp_text_input_v1_traits,
                     zwp_text_input_manager_v1_traits::Op::CreateTextInput>(
                     *mgr_.Get())))
      return false;
    ti_.Get()->backend_ = this;
    return true;
  }

  /// Set the surface that owns the active text field (v1 activate() argument).
  void SetSurface(wl_proxy* surface) noexcept { surface_ = surface; }

  /// Destroy the text_input and manager proxies.
  void Release() noexcept {
    ti_.Reset();
    mgr_.Reset();
  }

  // ── ITextInputReceiver
  // ──────────────────────────────────────────────────────
  void Activate() override {
    if (!ti_.IsNull())
      ti_.Get()->Activate(seat_, surface_);
  }
  void Deactivate() override {
    if (!ti_.IsNull())
      ti_.Get()->Deactivate(seat_);
  }
  void SetSurroundingText(std::string_view utf8,
                          uint32_t cursor,
                          uint32_t anchor) override {
    if (ti_.IsNull())
      return;
    // set_surrounding_text needs a NUL-terminated string; string_view is not.
    text_buf_.assign(utf8);
    ti_.Get()->SetSurroundingText(text_buf_.c_str(), cursor, anchor);
  }
  void SetContentType(ContentHint hint, ContentPurpose purpose) override {
    if (!ti_.IsNull())
      ti_.Get()->SetContentType(MapHint(hint), MapPurpose(purpose));
  }
  void SetCursorRectangle(int32_t x,
                          int32_t y,
                          int32_t width,
                          int32_t height) override {
    if (!ti_.IsNull())
      ti_.Get()->SetCursorRectangle(x, y, width, height);
  }
  void Reset() override {
    if (!ti_.IsNull())
      ti_.Get()->Reset();
  }
  void Commit() override {
    if (!ti_.IsNull())
      ti_.Get()->CommitState(serial_++);
  }
  void ShowPanel() override {
    if (!ti_.IsNull())
      ti_.Get()->ShowInputPanel();
  }
  void HidePanel() override {
    if (!ti_.IsNull())
      ti_.Get()->HideInputPanel();
  }

 private:
  // CRTP event handler: forwards zwp_text_input_v1 events to the listener.
  struct Handler
      : public text_input_unstable_v1::client::CZwpTextInputV1<Handler> {
    TextInputV1Backend* backend_ = nullptr;

    void OnEnter(wl_proxy* /*surface*/) override {
      if (backend_->listener_)
        backend_->listener_->OnEnter();
    }
    void OnLeave() override {
      if (backend_->listener_)
        backend_->listener_->OnLeave();
    }
    void OnCommitString(uint32_t serial, const char* text) override {
      backend_->serial_ = serial;
      if (backend_->listener_ && text)
        backend_->listener_->OnCommitString(text);
    }
    void OnPreeditString(uint32_t serial,
                         const char* text,
                         const char* /*commit*/) override {
      backend_->serial_ = serial;
      if (backend_->listener_ && text)
        backend_->listener_->OnPreeditString(text, backend_->preedit_cursor_,
                                             backend_->preedit_cursor_);
    }
    void OnPreeditCursor(int32_t index) override {
      backend_->preedit_cursor_ = index;
    }
    void OnDeleteSurroundingText(int32_t index, uint32_t length) override {
      // v1 index is relative to the cursor (negative = before it); split into
      // the facade's before/after byte counts.
      const uint32_t before = index < 0 ? static_cast<uint32_t>(-index) : 0u;
      const uint32_t after = length > before ? length - before : 0u;
      if (backend_->listener_)
        backend_->listener_->OnDeleteSurroundingText(before, after);
    }
    void OnKeysym(uint32_t serial,
                  uint32_t time,
                  uint32_t sym,
                  uint32_t state,
                  uint32_t modifiers) override {
      backend_->serial_ = serial;
      if (backend_->listener_)
        backend_->listener_->OnKeysym(time, sym, state, modifiers);
    }
  };

  struct Manager
      : public text_input_unstable_v1::client::CZwpTextInputManagerV1<Manager> {
  };

  // The facade ContentHint bit values intentionally coincide with v1's
  // individual content_hint bits, so this is an identity mapping.
  static uint32_t MapHint(ContentHint hint) noexcept {
    return static_cast<uint32_t>(hint);
  }

  static uint32_t MapPurpose(ContentPurpose purpose) noexcept {
    using V = text_input_unstable_v1::client::ZwpTextInputV1ContentPurpose;
    switch (purpose) {
      case ContentPurpose::kNormal:
        return static_cast<uint32_t>(V::Normal);
      case ContentPurpose::kAlpha:
        return static_cast<uint32_t>(V::Alpha);
      case ContentPurpose::kDigits:
        return static_cast<uint32_t>(V::Digits);
      case ContentPurpose::kNumber:
        return static_cast<uint32_t>(V::Number);
      case ContentPurpose::kPhone:
        return static_cast<uint32_t>(V::Phone);
      case ContentPurpose::kUrl:
        return static_cast<uint32_t>(V::Url);
      case ContentPurpose::kEmail:
        return static_cast<uint32_t>(V::Email);
      case ContentPurpose::kName:
        return static_cast<uint32_t>(V::Name);
      case ContentPurpose::kPassword:
      case ContentPurpose::kPin:  // v1 has no dedicated PIN purpose
        return static_cast<uint32_t>(V::Password);
      case ContentPurpose::kDate:
        return static_cast<uint32_t>(V::Date);
      case ContentPurpose::kTime:
        return static_cast<uint32_t>(V::Time);
      case ContentPurpose::kDatetime:
        return static_cast<uint32_t>(V::Datetime);
      case ContentPurpose::kTerminal:
        return static_cast<uint32_t>(V::Terminal);
    }
    return static_cast<uint32_t>(V::Normal);
  }

  // Declaration order = reverse destruction order: ti_ is destroyed before
  // mgr_.
  wl::WlPtr<Manager> mgr_;
  wl::WlPtr<Handler> ti_;

  TextInputListener* listener_ = nullptr;
  wl_proxy* seat_ = nullptr;
  wl_proxy* surface_ = nullptr;
  std::string text_buf_;
  uint32_t name_ = 0;
  uint32_t ver_adv_ = 0;
  uint32_t serial_ = 0;
  int32_t preedit_cursor_ = 0;
};

}  // namespace wl::ime
