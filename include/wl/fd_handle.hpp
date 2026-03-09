// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
#pragma once
#include <unistd.h>
#include <utility>

namespace wl {

/// RAII wrapper for a POSIX file descriptor.
class FdHandle {
 public:
  FdHandle() noexcept = default;
  explicit FdHandle(int fd) noexcept : m_fd(fd) {}
  FdHandle(FdHandle&& o) noexcept : m_fd(std::exchange(o.m_fd, -1)) {}
  FdHandle& operator=(FdHandle&& o) noexcept {
    if (this != &o) {
      Close();
      m_fd = std::exchange(o.m_fd, -1);
    }
    return *this;
  }
  FdHandle(const FdHandle&) = delete;
  FdHandle& operator=(const FdHandle&) = delete;
  ~FdHandle() noexcept { Close(); }

  [[nodiscard]] int Get() const noexcept { return m_fd; }
  [[nodiscard]] bool IsNull() const noexcept { return m_fd < 0; }
  explicit operator bool() const noexcept { return !IsNull(); }
  [[nodiscard]] int Detach() noexcept { return std::exchange(m_fd, -1); }
  void Close() noexcept {
    if (m_fd >= 0)
      ::close(std::exchange(m_fd, -1));
  }

 private:
  int m_fd = -1;
};

}  // namespace wl
