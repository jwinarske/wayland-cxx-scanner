// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
// Backend dispatch header: resolves the build-time IME backend selection
// (the WL_IME_BACKEND_* define set by the `ime_backend` meson option) to one
// concrete type, aliased to its role facade, so consumers never name a backend
// directly.  Exactly one backend is selected per build.
//
// With ime_backend=none (WL_IME_BACKEND_NONE) this header pulls in nothing and
// defines no alias; the facade contracts in this directory are still usable.
#if defined(WL_IME_BACKEND_TEXT_INPUT_V1)
#include <wl/ime/backends/text_input_v1.hpp>
namespace wl::ime {
using SelectedTextInput = TextInputV1Backend;
}
#elif defined(WL_IME_BACKEND_TEXT_INPUT_V3)
#include <wl/ime/backends/text_input_v3.hpp>
namespace wl::ime {
using SelectedTextInput = TextInputV3Backend;
}
#elif defined(WL_IME_BACKEND_INPUT_METHOD_V1)
#include <wl/ime/backends/input_method_v1.hpp>
namespace wl::ime {
using SelectedInputMethod = InputMethodV1Backend;
}
#elif defined(WL_IME_BACKEND_INPUT_METHOD_V2)
#include <wl/ime/backends/input_method_v2.hpp>
namespace wl::ime {
using SelectedInputMethod = InputMethodV2Backend;
}
#elif defined(WL_IME_BACKEND_VIRTUAL_KEYBOARD_V1)
#include <wl/ime/backends/virtual_keyboard_v1.hpp>
namespace wl::ime {
using SelectedVirtualKeyboard = VirtualKeyboardV1Backend;
}
#elif !defined(WL_IME_BACKEND_NONE)
#error \
    "No IME backend selected; set ime_backend in meson or define WL_IME_BACKEND_NONE."
#endif
