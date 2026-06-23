// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

extern "C" {
#include <wayland-client-core.h>
}

namespace wl {

namespace detail {

/// Detection trait backing WlProxyTraits (C++17 substitute for a concept).
template <typename T, typename = void>
struct is_wl_proxy_traits : std::false_type {};

template <typename T>
struct is_wl_proxy_traits<T,
                          std::void_t<decltype(T::interface_name),
                                      decltype(T::version),
                                      decltype(T::wl_iface())>>
    : std::bool_constant<
          std::is_convertible_v<decltype(T::interface_name),
                                std::string_view> &&
          std::is_convertible_v<decltype(T::version), uint32_t> &&
          std::is_same_v<decltype(T::wl_iface()), const wl_interface&>> {};

}  // namespace detail

/// True for any Wayland interface traits struct.  C++17 substitute for the
/// former C++20 concept; semantics unchanged.  Traits must provide:
///   - static constexpr std::string_view interface_name
///   - static constexpr uint32_t         version
///   - static const wl_interface& wl_iface() noexcept
template <typename T>
inline constexpr bool WlProxyTraits = detail::is_wl_proxy_traits<T>::value;

/// Non-owning, type-safe handle wrapper for a wl_proxy* (≈ WTL CWindow).
///
/// @tparam Traits  An interface traits struct satisfying WlProxyTraits.
template <typename Traits>
class CProxy {
  static_assert(WlProxyTraits<Traits>,
                "Traits must satisfy wl::WlProxyTraits "
                "(interface_name, version, wl_iface())");

 public:
  CProxy() noexcept = default;

  // Non-copyable, non-movable — it is a lightweight handle.
  CProxy(const CProxy&) = delete;
  CProxy& operator=(const CProxy&) = delete;
  CProxy(CProxy&&) = delete;
  CProxy& operator=(CProxy&&) = delete;

  /// Attach an externally created proxy. Asserts the handle is currently null.
  void Attach(wl_proxy* p) noexcept { m_proxy = p; }

  /// Detach and return the raw proxy without destroying it.
  wl_proxy* Detach() noexcept {
    wl_proxy* p = m_proxy;
    m_proxy = nullptr;
    return p;
  }

  /// Destroy the underlying proxy (calls wl_proxy_destroy).
  void Destroy() noexcept {
    if (m_proxy)
      wl_proxy_destroy(std::exchange(m_proxy, nullptr));
  }

  [[nodiscard]] wl_proxy* GetProxy() const noexcept { return m_proxy; }
  [[nodiscard]] bool IsNull() const noexcept { return m_proxy == nullptr; }
  explicit operator bool() const noexcept { return !IsNull(); }

  [[nodiscard]] static constexpr std::string_view InterfaceName() noexcept {
    return Traits::interface_name;
  }
  [[nodiscard]] static constexpr uint32_t MaxVersion() noexcept {
    return Traits::version;
  }
  [[nodiscard]] static const wl_interface& WlInterface() noexcept {
    return Traits::wl_iface();
  }

 protected:
  wl_proxy* m_proxy = nullptr;

 private:
  // Allow exchange in move operations inside WlPtr.
  template <typename U>
  friend class WlPtr;
  wl_proxy* _Exchange(wl_proxy* p) noexcept {
    return std::exchange(m_proxy, p);
  }
};

}  // namespace wl
