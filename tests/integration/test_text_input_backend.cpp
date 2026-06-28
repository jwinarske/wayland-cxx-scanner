// SPDX-License-Identifier: MIT
// Built for the text-input receiver backends (-Dime_backend=text-input-v1 or
// text-input-v3).  Compiles the selected backend (via the backend dispatch
// header) against the ITextInputReceiver facade and exercises every facade
// method.  Methods are safe no-ops while the backend is unbound, so this needs
// no live compositor; instantiating the non-template backend forces its whole
// implementation (bind path, event handler, enum mapping) to compile.
#include <wl/ime/backend.hpp>
#include <wl/ime/text_input_receiver.hpp>

#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

static_assert(
    std::is_base_of_v<wl::ime::ITextInputReceiver, wl::ime::SelectedTextInput>,
    "the selected text-input backend must implement ITextInputReceiver");

namespace {
struct FakeListener : wl::ime::TextInputListener {
  void OnEnter() override {}
  void OnLeave() override {}
  void OnCommitString(std::string_view) override {}
  void OnPreeditString(std::string_view, int32_t, int32_t) override {}
  void OnDeleteSurroundingText(uint32_t, uint32_t) override {}
};
}  // namespace

TEST(TextInputBackend, FacadeMethodsSafeWhenUnbound) {
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

#if defined(WL_IME_BACKEND_TEXT_INPUT_V3)
TEST(TextInputBackend, NilPreeditCoalescedToEmpty) {
  // A nil preedit string is zwp_text_input_v3's "clear preedit" signal: it must
  // be forwarded as empty, not dropped. Dropping it leaves stale preedit on
  // screen (e.g. backspace over a one-character composing run does nothing).
  EXPECT_TRUE(wl::ime::CoalescePreedit(nullptr).empty());
  EXPECT_EQ(wl::ime::CoalescePreedit("ni"), std::string_view{"ni"});
}
#endif
