// SPDX-License-Identifier: MIT
// Built only when -Dime_backend=text-input-v1.  Compiles the text-input-v1
// backend (via the backend dispatch header) against the ITextInputReceiver
// facade and exercises every facade method.  Methods are safe no-ops while the
// backend is unbound, so this needs no live compositor; instantiating the
// non-template backend forces its whole implementation (bind path, event
// handler, enum mapping) to compile.
#include <wl/ime/backend.hpp>
#include <wl/ime/text_input_receiver.hpp>

#include <type_traits>

#include <gtest/gtest.h>

static_assert(
    std::is_base_of_v<wl::ime::ITextInputReceiver, wl::ime::SelectedTextInput>,
    "the text-input-v1 backend must implement ITextInputReceiver");

namespace {
struct FakeListener : wl::ime::TextInputListener {
  void OnEnter() override {}
  void OnLeave() override {}
  void OnCommitString(std::string_view) override {}
  void OnPreeditString(std::string_view, int32_t, int32_t) override {}
  void OnDeleteSurroundingText(uint32_t, uint32_t) override {}
};
}  // namespace

TEST(TextInputV1Backend, FacadeMethodsSafeWhenUnbound) {
  wl::ime::SelectedTextInput ti;
  wl::ime::ITextInputReceiver& r = ti;
  r.Activate();
  r.Deactivate();
  r.SetSurroundingText("hello", 5, 5);
  r.SetContentType(
      wl::ime::ContentHint::kSpellcheck | wl::ime::ContentHint::kLatin,
      wl::ime::ContentPurpose::kEmail);
  r.SetCursorRectangle(0, 0, 100, 20);
  r.Reset();
  r.Commit();
  r.ShowPanel();
  r.HidePanel();
  SUCCEED();
}
