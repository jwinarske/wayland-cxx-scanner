// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <wl/raii.hpp>

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

extern "C" {
// wl_display, wl_registry, wl_registry_listener, wl_registry_bind, etc.
#include <wayland-client.h>
// wl_client, wl_global, wl_global_create, wl_global_destroy
#include <wayland-server-core.h>
}

namespace wl {

/// Client-side Wayland registry wrapper.
///
/// Wraps wl_registry and exposes typed callbacks for global advertisement
/// and removal events.
class CRegistry {
 public:
  using GlobalFn =
      std::function<void(CRegistry&, uint32_t, std::string_view, uint32_t)>;
  using RemoveFn = std::function<void(CRegistry&, uint32_t)>;

  CRegistry() noexcept = default;
  ~CRegistry() { _Reset(); }

  CRegistry(const CRegistry&) = delete;
  CRegistry& operator=(const CRegistry&) = delete;
  CRegistry(CRegistry&&) = delete;
  CRegistry& operator=(CRegistry&&) = delete;

  /// Create the registry from a connected display.
  /// Returns true on success.
  [[nodiscard]] bool Create(wl_display* display) noexcept {
    _Reset();
    m_registry = wl_display_get_registry(display);
    if (!m_registry)
      return false;
    wl_registry_add_listener(m_registry, &s_listener_, this);
    return true;
  }

  [[nodiscard]] bool IsNull() const noexcept { return m_registry == nullptr; }
  [[nodiscard]] wl_registry* Get() const noexcept { return m_registry; }
  explicit operator bool() const noexcept { return !IsNull(); }

  /// Explicitly destroy the registry.
  /// Must be called before wl_display_disconnect() to avoid use-after-free:
  /// wl_registry_destroy() (called from the destructor) accesses display->mutex
  /// inside libwayland, so the registry must be gone before the display is
  /// freed.
  void Reset() noexcept { _Reset(); }

  /// Set callback invoked for each wl_registry.global event.
  void OnGlobal(GlobalFn fn) { m_on_global = std::move(fn); }

  /// Set callback invoked for each wl_registry.global_remove event.
  void OnRemove(RemoveFn fn) { m_on_remove = std::move(fn); }

  /// Bind a global object by name.
  /// Returns nullptr when the registry handle is null (S5).
  template <typename Traits>
  [[nodiscard]] wl_proxy* Bind(uint32_t name, uint32_t version) noexcept {
    if (!m_registry)
      return nullptr;
    return static_cast<wl_proxy*>(
        wl_registry_bind(m_registry, name, &Traits::wl_iface(), version));
  }

 private:
  wl_registry* m_registry = nullptr;
  GlobalFn m_on_global;
  RemoveFn m_on_remove;

  void _Reset() noexcept {
    if (m_registry)
      wl_registry_destroy(std::exchange(m_registry, nullptr));
  }

  static void _OnGlobal(void* data,
                        wl_registry* /*reg*/,
                        uint32_t name,
                        const char* interface,
                        uint32_t version) {
    auto* self = static_cast<CRegistry*>(data);
    if (self->m_on_global)
      self->m_on_global(*self, name, std::string_view{interface}, version);
  }

  static void _OnGlobalRemove(void* data, wl_registry* /*reg*/, uint32_t name) {
    auto* self = static_cast<CRegistry*>(data);
    if (self->m_on_remove)
      self->m_on_remove(*self, name);
  }

  static constexpr wl_registry_listener s_listener_ = {
      .global = _OnGlobal,
      .global_remove = _OnGlobalRemove,
  };
};

/// Server-side global factory for a single interface (≈ WTL CComCoClass).
///
/// @tparam Traits  Server-side interface traits struct providing
///                 interface_name, version, and wl_iface().
template <typename Traits>
class CGlobal {
 public:
  CGlobal() noexcept = default;
  ~CGlobal() { _Reset(); }

  CGlobal(const CGlobal&) = delete;
  CGlobal& operator=(const CGlobal&) = delete;
  CGlobal(CGlobal&&) = delete;
  CGlobal& operator=(CGlobal&&) = delete;

  /// Advertise the global on the display.
  [[nodiscard]] bool Create(wl_display* display, uint32_t version) noexcept {
    _Reset();
    m_global = wl_global_create(display, &Traits::wl_iface(),
                                static_cast<int>(version), this, _OnBind);
    return m_global != nullptr;
  }

  [[nodiscard]] bool IsNull() const noexcept { return m_global == nullptr; }
  [[nodiscard]] wl_global* Get() const noexcept { return m_global; }

 private:
  wl_global* m_global = nullptr;

  void _Reset() noexcept {
    if (m_global)
      wl_global_destroy(std::exchange(m_global, nullptr));
  }

  static void _OnBind(wl_client* /*client*/,
                      void* /*data*/,
                      uint32_t /*version*/,
                      uint32_t /*id*/) {
    // Subclasses override by providing a bind callback via CGlobal
    // specialisation.
  }
};

}  // namespace wl
