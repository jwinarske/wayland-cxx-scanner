// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <system_error>
#include <utility>

namespace wl {

/// RAII wrapper for a C FILE pointer.
class FileHandle {
 public:
  FileHandle() noexcept = default;

  explicit FileHandle(const std::filesystem::path& p, const char* mode) {
    m_file = std::fopen(p.c_str(), mode);
    if (!m_file)
      throw std::system_error(errno, std::generic_category(),
                              "cannot open " + p.string());
  }

  /// Tag type for adopting a non-owned FILE* (e.g. stdin/stdout/stderr).
  struct adopt_raw_t {};
  static constexpr adopt_raw_t adopt_raw{};

  FileHandle(adopt_raw_t /*tag*/, FILE* f) noexcept
      : m_file(f), m_owned(false) {}

  FileHandle(FileHandle&& o) noexcept
      : m_file(std::exchange(o.m_file, nullptr)),
        m_owned(std::exchange(o.m_owned, true)) {}

  FileHandle& operator=(FileHandle&& o) noexcept {
    if (this != &o) {
      _Close();
      m_file = std::exchange(o.m_file, nullptr);
      m_owned = std::exchange(o.m_owned, true);
    }
    return *this;
  }

  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  ~FileHandle() noexcept { _Close(); }

  [[nodiscard]] FILE* Get() const noexcept { return m_file; }
  [[nodiscard]] bool IsNull() const noexcept { return !m_file; }
  explicit operator bool() const noexcept { return !IsNull(); }
  [[nodiscard]] FILE* Detach() noexcept {
    return std::exchange(m_file, nullptr);
  }

 private:
  FILE* m_file = nullptr;
  bool m_owned = true;

  void _Close() noexcept {
    if (m_file && m_owned)
      std::fclose(std::exchange(m_file, nullptr));
    else
      m_file = nullptr;
  }
};

/// Executes a callable when the scope exits (RAII scope guard).
template <typename F>
class [[nodiscard]] ScopeExit {
 public:
  explicit ScopeExit(F f) noexcept : m_fn(std::move(f)) {}
  ~ScopeExit() { m_fn(); }
  ScopeExit(ScopeExit&&) = delete;
  ScopeExit(const ScopeExit&) = delete;

 private:
  F m_fn;
};
template <typename F>
ScopeExit(F) -> ScopeExit<F>;

}  // namespace wl
