// SPDX-License-Identifier: MIT
// Compile/contract test for the IME role facades and the backend dispatch
// header. Builds with ime_backend=none (WL_IME_BACKEND_NONE), so backend.hpp
// pulls in no backend; the facade contracts must still be complete and usable.
#include <wl/ime/backend.hpp>
#include <wl/ime/input_method.hpp>
#include <wl/ime/text_input_receiver.hpp>
#include <wl/ime/virtual_keyboard.hpp>

#include <gtest/gtest.h>

namespace {
using namespace wl::ime;

struct TextInputImpl : ITextInputReceiver, TextInputListener {
  void Activate() override {}
  void Deactivate() override {}
  void SetSurroundingText(std::string_view, uint32_t, uint32_t) override {}
  void SetContentType(ContentHint, ContentPurpose) override {}
  void SetCursorRectangle(int32_t, int32_t, int32_t, int32_t) override {}
  void Reset() override {}
  void Commit() override {}
  void ShowPanel() override {}
  void HidePanel() override {}
  void OnEnter() override {}
  void OnLeave() override {}
  void OnCommitString(std::string_view) override {}
  void OnPreeditString(std::string_view, int32_t, int32_t) override {}
  void OnDeleteSurroundingText(uint32_t, uint32_t) override {}
};

struct InputMethodImpl : IInputMethod, InputMethodListener {
  void CommitString(std::string_view) override {}
  void SetPreeditString(std::string_view, int32_t, int32_t) override {}
  void DeleteSurroundingText(uint32_t, uint32_t) override {}
  void Commit() override {}
  void OnActivate() override {}
  void OnDeactivate() override {}
  void OnSurroundingText(std::string_view, uint32_t, uint32_t) override {}
  void OnContentType(ContentHint, ContentPurpose) override {}
  void OnDone() override {}
};

struct VirtualKeyboardImpl : IVirtualKeyboard {
  void SetKeymap(int, uint32_t, uint32_t) override {}
  void Key(uint32_t, uint32_t, uint32_t) override {}
  void Modifiers(uint32_t, uint32_t, uint32_t, uint32_t) override {}
};
}  // namespace

TEST(ImeFacades, InterfacesAreImplementable) {
  TextInputImpl ti;
  InputMethodImpl im;
  VirtualKeyboardImpl vk;
  ITextInputReceiver& r = ti;
  IInputMethod& m = im;
  IVirtualKeyboard& k = vk;
  (void)r;
  (void)m;
  (void)k;
  SUCCEED();
}

TEST(ImeFacades, ContentHintFlags) {
  constexpr auto h = ContentHint::kSpellcheck | ContentHint::kLatin;
  EXPECT_TRUE((h & ContentHint::kSpellcheck) == ContentHint::kSpellcheck);
  EXPECT_TRUE((h & ContentHint::kLatin) == ContentHint::kLatin);
  EXPECT_TRUE((h & ContentHint::kUppercase) == ContentHint::kNone);
}
