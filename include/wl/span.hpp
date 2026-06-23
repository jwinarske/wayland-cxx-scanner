// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
// Minimal C++17 substitute for std::span (dynamic-extent, mutable).
//
// The framework and its examples target a C++17 floor; std::span is C++20.
// This provides exactly the small subset the examples rely on (pointer/size
// construction, iteration, indexing, first()/last()/subspan()) without pulling
// in C++20.  It is intentionally not a full std::span implementation.
#include <cstddef>
#include <type_traits>
#include <utility>

namespace wl {

template <typename T>
class span {
 public:
  using element_type = T;
  using value_type = std::remove_cv_t<T>;
  using size_type = std::size_t;
  using pointer = T*;
  using reference = T&;
  using iterator = T*;

  constexpr span() noexcept = default;
  constexpr span(pointer ptr, size_type count) noexcept
      : data_(ptr), size_(count) {}

  // Implicit construction from a contiguous container (e.g. std::vector,
  // std::array), mirroring std::span.  SFINAE'd out for span itself so the
  // copy constructor is preferred.
  template <
      typename Container,
      typename = std::enable_if_t<
          !std::is_same_v<std::remove_cv_t<Container>, span> &&
          std::is_convertible_v<decltype(std::declval<Container&>().data()),
                                pointer>>>
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  constexpr span(Container& c) noexcept : data_(c.data()), size_(c.size()) {}

  [[nodiscard]] constexpr pointer data() const noexcept { return data_; }
  [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  [[nodiscard]] constexpr reference operator[](size_type i) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return data_[i];
  }

  [[nodiscard]] constexpr iterator begin() const noexcept { return data_; }
  [[nodiscard]] constexpr iterator end() const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return data_ + size_;
  }

  [[nodiscard]] constexpr span first(size_type n) const noexcept {
    return span(data_, n);
  }
  [[nodiscard]] constexpr span last(size_type n) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return span(data_ + (size_ - n), n);
  }
  [[nodiscard]] constexpr span subspan(size_type offset) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return span(data_ + offset, size_ - offset);
  }

 private:
  pointer data_ = nullptr;
  size_type size_ = 0;
};

}  // namespace wl
