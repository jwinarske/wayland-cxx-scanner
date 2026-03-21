// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// key-input — text-editor keyboard-repeat demonstration
//
// Exercises wl::KeyboardHandler keyboard repeat, modelled after
// weston/clients/editor.c: text buffer with cursor, printable-character
// insertion via xkb_keysym_to_utf32, backspace/delete, arrow keys (with
// keyboard repeat), Home, End, Enter.
//
// Rendering uses wl_shm (XRGB8888) with an inline 8×8 bitmap pixel font; no
// EGL/Mesa/GPU dependency.  App structure mirrors simple-egl exactly:
//   ConnectDisplay → ScanGlobals → BindGlobals → CreateSurfaces →
//   InitShm → MainLoop
//
// Build requirements: wayland-client, wayland-protocols, xkbcommon.
// Runtime requirement: a running Wayland compositor with xdg-shell support.

// ── Generated C++ protocol headers ───────────────────────────────────────────
// wayland_client.hpp   → namespace wayland::client  (from wayland.xml)
// xdg_shell_client.hpp → namespace xdg_shell::client (from xdg-shell.xml)
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── System headers
// ────────────────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>  // KEY_ESC, KEY_BACKSPACE, …
#include <sys/mman.h>                 // memfd_create, mmap, munmap
#include <unistd.h>                   // close, ftruncate
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>  // xkb_keysym_to_utf32
}

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <utility>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// wl_seat_traits::wl_iface() and wl_keyboard_traits::wl_iface() are provided
// inline by <wl/seat.hpp>.
// All xdg_shell traits wl_iface() implementations are provided inline by
// <wl/xdg_shell.hpp>.
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

const wl_interface& wl_compositor_traits::wl_iface() noexcept {
  return wl_compositor_interface;
}
const wl_interface& wl_surface_traits::wl_iface() noexcept {
  return wl_surface_interface;
}
const wl_interface& wl_shm_traits::wl_iface() noexcept {
  return wl_shm_interface;
}
const wl_interface& wl_shm_pool_traits::wl_iface() noexcept {
  return wl_shm_pool_interface;
}
const wl_interface& wl_buffer_traits::wl_iface() noexcept {
  return wl_buffer_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// Inline 8×8 bitmap font — printable ASCII (0x20 … 0x7E, 95 glyphs)
//
// Each entry is 8 bytes, one per row (top to bottom).  Within each byte
// bit 7 is the leftmost pixel and bit 0 is the rightmost.  The glyph occupies
// bits 6–1 (a 6-pixel-wide column within the 8-pixel cell), leaving 1-pixel
// margins on each side.
//
// Bit-to-column mapping for the 6-pixel glyph area:
//   bit 6 = col 0 (leftmost)  …  bit 1 = col 5 (rightmost)
// Full-width horizontal bar = 0x7E; empty row = 0x00.
// ══════════════════════════════════════════════════════════════════════════════

// clang-format off
static constexpr uint8_t kGlyph[95][8] = {
  /* 0x20   */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  /* 0x21 ! */ {0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
  /* 0x22 " */ {0x00,0x66,0x66,0x24,0x00,0x00,0x00,0x00},
  /* 0x23 # */ {0x00,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x00},
  /* 0x24 $ */ {0x10,0x7C,0xD0,0x7C,0x16,0x7C,0x10,0x00},
  /* 0x25 % */ {0x00,0x66,0x6C,0x18,0x36,0x66,0x00,0x00},
  /* 0x26 & */ {0x00,0x38,0x6C,0x38,0x6E,0x66,0x3E,0x00},
  /* 0x27 ' */ {0x00,0x18,0x18,0x10,0x00,0x00,0x00,0x00},
  /* 0x28 ( */ {0x00,0x0E,0x1C,0x18,0x18,0x1C,0x0E,0x00},
  /* 0x29 ) */ {0x00,0x70,0x38,0x18,0x18,0x38,0x70,0x00},
  /* 0x2A * */ {0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00},
  /* 0x2B + */ {0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00},
  /* 0x2C , */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
  /* 0x2D - */ {0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00},
  /* 0x2E . */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
  /* 0x2F / */ {0x00,0x06,0x0C,0x18,0x30,0x60,0x00,0x00},
  /* 0x30 0 */ {0x00,0x3C,0x66,0x6E,0x76,0x66,0x3C,0x00},
  /* 0x31 1 */ {0x00,0x18,0x38,0x18,0x18,0x18,0x7E,0x00},
  /* 0x32 2 */ {0x00,0x3C,0x66,0x06,0x1C,0x30,0x7E,0x00},
  /* 0x33 3 */ {0x00,0x7E,0x0C,0x18,0x0C,0x66,0x3C,0x00},
  /* 0x34 4 */ {0x00,0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x00},
  /* 0x35 5 */ {0x00,0x7E,0x60,0x7C,0x06,0x66,0x3C,0x00},
  /* 0x36 6 */ {0x00,0x1C,0x30,0x7C,0x66,0x66,0x3C,0x00},
  /* 0x37 7 */ {0x00,0x7E,0x06,0x0C,0x18,0x18,0x18,0x00},
  /* 0x38 8 */ {0x00,0x3C,0x66,0x3C,0x66,0x66,0x3C,0x00},
  /* 0x39 9 */ {0x00,0x3C,0x66,0x66,0x3E,0x06,0x3C,0x00},
  /* 0x3A : */ {0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x00},
  /* 0x3B ; */ {0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x30},
  /* 0x3C < */ {0x00,0x0C,0x18,0x30,0x18,0x0C,0x00,0x00},
  /* 0x3D = */ {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
  /* 0x3E > */ {0x00,0x30,0x18,0x0C,0x18,0x30,0x00,0x00},
  /* 0x3F ? */ {0x00,0x3C,0x66,0x0C,0x18,0x00,0x18,0x00},
  /* 0x40 @ */ {0x00,0x3C,0x66,0x6E,0x6E,0x60,0x3C,0x00},
  /* 0x41 A */ {0x00,0x18,0x3C,0x66,0x7E,0x66,0x66,0x00},
  /* 0x42 B */ {0x00,0x7C,0x66,0x7C,0x66,0x66,0x7C,0x00},
  /* 0x43 C */ {0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00},
  /* 0x44 D */ {0x00,0x78,0x6C,0x66,0x66,0x6C,0x78,0x00},
  /* 0x45 E */ {0x00,0x7E,0x60,0x7C,0x60,0x60,0x7E,0x00},
  /* 0x46 F */ {0x00,0x7E,0x60,0x7C,0x60,0x60,0x60,0x00},
  /* 0x47 G */ {0x00,0x3C,0x66,0x60,0x6E,0x66,0x3C,0x00},
  /* 0x48 H */ {0x00,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
  /* 0x49 I */ {0x00,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00},
  /* 0x4A J */ {0x00,0x1E,0x06,0x06,0x06,0x66,0x3C,0x00},
  /* 0x4B K */ {0x00,0x66,0x6C,0x78,0x78,0x6C,0x66,0x00},
  /* 0x4C L */ {0x00,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
  /* 0x4D M */ {0x00,0x66,0x7E,0x7E,0x66,0x66,0x66,0x00},
  /* 0x4E N */ {0x00,0x66,0x76,0x7E,0x6E,0x66,0x66,0x00},
  /* 0x4F O */ {0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00},
  /* 0x50 P */ {0x00,0x7C,0x66,0x66,0x7C,0x60,0x60,0x00},
  /* 0x51 Q */ {0x00,0x3C,0x66,0x66,0x66,0x6C,0x36,0x00},
  /* 0x52 R */ {0x00,0x7C,0x66,0x66,0x7C,0x6C,0x66,0x00},
  /* 0x53 S */ {0x00,0x3E,0x60,0x3C,0x06,0x06,0x7C,0x00},
  /* 0x54 T */ {0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x00},
  /* 0x55 U */ {0x00,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
  /* 0x56 V */ {0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
  /* 0x57 W */ {0x00,0x66,0x66,0x66,0x7E,0x7E,0x66,0x00},
  /* 0x58 X */ {0x00,0x66,0x66,0x3C,0x3C,0x66,0x66,0x00},
  /* 0x59 Y */ {0x00,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
  /* 0x5A Z */ {0x00,0x7E,0x06,0x1C,0x30,0x60,0x7E,0x00},
  /* 0x5B [ */ {0x00,0x1E,0x18,0x18,0x18,0x18,0x1E,0x00},
  /* 0x5C \ */ {0x00,0x60,0x30,0x18,0x0C,0x06,0x00,0x00},
  /* 0x5D ] */ {0x00,0x78,0x18,0x18,0x18,0x18,0x78,0x00},
  /* 0x5E ^ */ {0x00,0x18,0x3C,0x66,0x00,0x00,0x00,0x00},
  /* 0x5F _ */ {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00},
  /* 0x60 ` */ {0x00,0x30,0x18,0x00,0x00,0x00,0x00,0x00},
  /* 0x61 a */ {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
  /* 0x62 b */ {0x00,0x60,0x60,0x7C,0x66,0x66,0x7C,0x00},
  /* 0x63 c */ {0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00},
  /* 0x64 d */ {0x00,0x06,0x06,0x3E,0x66,0x66,0x3E,0x00},
  /* 0x65 e */ {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
  /* 0x66 f */ {0x00,0x1C,0x30,0x7C,0x30,0x30,0x30,0x00},
  /* 0x67 g */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
  /* 0x68 h */ {0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x00},
  /* 0x69 i */ {0x00,0x18,0x00,0x38,0x18,0x18,0x3C,0x00},
  /* 0x6A j */ {0x00,0x06,0x00,0x06,0x06,0x66,0x66,0x3C},
  /* 0x6B k */ {0x00,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
  /* 0x6C l */ {0x00,0x38,0x18,0x18,0x18,0x18,0x3C,0x00},
  /* 0x6D m */ {0x00,0x00,0x66,0x7E,0x7E,0x66,0x66,0x00},
  /* 0x6E n */ {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
  /* 0x6F o */ {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
  /* 0x70 p */ {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
  /* 0x71 q */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
  /* 0x72 r */ {0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x00},
  /* 0x73 s */ {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
  /* 0x74 t */ {0x00,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00},
  /* 0x75 u */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
  /* 0x76 v */ {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
  /* 0x77 w */ {0x00,0x00,0x66,0x66,0x7E,0x7E,0x66,0x00},
  /* 0x78 x */ {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
  /* 0x79 y */ {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},
  /* 0x7A z */ {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},
  /* 0x7B { */ {0x00,0x0E,0x18,0x70,0x18,0x18,0x0E,0x00},
  /* 0x7C | */ {0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
  /* 0x7D } */ {0x00,0x70,0x18,0x0E,0x18,0x18,0x70,0x00},
  /* 0x7E ~ */ {0x00,0x00,0x32,0x7E,0x4C,0x00,0x00,0x00},
};
// clang-format on

// ══════════════════════════════════════════════════════════════════════════════
// TextBuffer — UTF-8-clean text buffer (ASCII subset) with cursor
//
// Models weston/editor.c: holds a std::string with '\n' line separators and a
// byte-offset cursor.  Only printable ASCII (0x20–0x7E) and '\n' are stored;
// the font covers exactly that range.
// ══════════════════════════════════════════════════════════════════════════════

class TextBuffer {
 public:
  explicit TextBuffer(const char* initial = "")
      : text_(initial), cursor_(text_.size()) {}

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

  /// Insert printable ASCII character or newline at the cursor.
  void Insert(char c) {
    text_.insert(cursor_, 1, c);
    ++cursor_;
  }

  /// Delete the character immediately before the cursor (backspace).
  void Backspace() noexcept {
    if (cursor_ == 0)
      return;
    --cursor_;
    text_.erase(cursor_, 1);
  }

  /// Delete the character immediately after the cursor (forward delete).
  void DeleteForward() noexcept {
    if (cursor_ < text_.size())
      text_.erase(cursor_, 1);
  }

  /// Move the cursor one character to the left.
  void MoveLeft() noexcept {
    if (cursor_ > 0)
      --cursor_;
  }

  /// Move the cursor one character to the right.
  void MoveRight() noexcept {
    if (cursor_ < text_.size())
      ++cursor_;
  }

  /// Move to the beginning of the current logical line ('\n'-delimited).
  void MoveHome() noexcept {
    if (cursor_ == 0)
      return;
    const auto pos = text_.rfind('\n', cursor_ - 1);
    cursor_ = (pos == std::string::npos) ? 0u : pos + 1;
  }

  /// Move to the end of the current logical line.
  void MoveEnd() noexcept {
    const auto pos = text_.find('\n', cursor_);
    cursor_ = (pos == std::string::npos) ? text_.size() : pos;
  }

  /// Move the cursor to the same column on the line above.
  void MoveUp() noexcept {
    if (cursor_ == 0)
      return;
    // Find start of current logical line.
    const auto nl = text_.rfind('\n', cursor_ - 1);
    const std::size_t line_start = (nl == std::string::npos) ? 0u : nl + 1;
    if (line_start == 0)
      return;  // already on first line
    const std::size_t col = cursor_ - line_start;
    // Find the '\n' that terminates the previous line.
    const std::size_t prev_nl_pos = line_start - 1;
    std::size_t prev_start = 0;
    if (prev_nl_pos > 0) {
      const auto pnl = text_.rfind('\n', prev_nl_pos - 1);
      prev_start = (pnl == std::string::npos) ? 0u : pnl + 1;
    }
    const std::size_t prev_len = prev_nl_pos - prev_start;
    cursor_ = prev_start + std::min(col, prev_len);
  }

  /// Move the cursor to the same column on the line below.
  void MoveDown() noexcept {
    const auto nl = text_.find('\n', cursor_);
    if (nl == std::string::npos) {
      cursor_ = text_.size();
      return;
    }
    // Compute column on the current line.
    const auto snl =
        (cursor_ > 0) ? text_.rfind('\n', cursor_ - 1) : std::string::npos;
    const std::size_t line_start = (snl == std::string::npos) ? 0u : snl + 1;
    const std::size_t col = cursor_ - line_start;
    // Advance to next line and clamp to its length.
    const std::size_t next_start = nl + 1;
    const auto next_nl = text_.find('\n', next_start);
    const std::size_t next_len = (next_nl == std::string::npos)
                                     ? text_.size() - next_start
                                     : next_nl - next_start;
    cursor_ = next_start + std::min(col, next_len);
  }

  /// Return the 0-based (line, column) of the cursor in logical-line space.
  [[nodiscard]] std::pair<int, int> CursorPos() const noexcept {
    int line = 0, col = 0;
    for (std::size_t i = 0; i < cursor_; ++i) {
      if (text_[i] == '\n') {
        ++line;
        col = 0;
      } else {
        ++col;
      }
    }
    return {line, col};
  }

 private:
  std::string text_;
  std::size_t cursor_ = 0;
};

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

// Forward-declare App so handler callbacks can call back into it.
class App;

// ── WlCompositorHandler ──────────────────────────────────────────────────────

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {
 public:
};

// ── WlSurfaceHandler ─────────────────────────────────────────────────────────

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

// ── WlShmPoolHandler ─────────────────────────────────────────────────────────
// wl_shm_pool has no events; only needed transiently during buffer creation.

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
 public:
};

// ── WlShmHandler ─────────────────────────────────────────────────────────────

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  uint32_t formats = 0;
  void OnFormat(uint32_t fmt) override {
    if (fmt < 32u)
      formats |= (1u << fmt);
  }
};

// ── WlBufferHandler ──────────────────────────────────────────────────────────
// Tracks whether the compositor is still using this buffer.

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool busy = false;
  void OnRelease() override { busy = false; }
};

// ══════════════════════════════════════════════════════════════════════════════
// SHM double-buffer state
//
// Two pixel buffers are allocated from a single shared-memory pool.  On each
// Redraw() call the next non-busy buffer is selected, rendered into, and
// committed.  WlBufferHandler::OnRelease() clears the busy flag when the
// compositor is done with a buffer.
// ══════════════════════════════════════════════════════════════════════════════

struct ShmBuffer {
  wl::WlPtr<WlBufferHandler> buf;
  uint8_t* pixels = nullptr;
};

struct ShmState {
  int fd = -1;
  void* data = MAP_FAILED;
  std::size_t total = 0;
  std::array<ShmBuffer, 2> bufs{};

  ShmState() = default;
  ~ShmState() noexcept { Reset(); }
  ShmState(const ShmState&) = delete;
  ShmState& operator=(const ShmState&) = delete;
  ShmState(ShmState&&) = delete;
  ShmState& operator=(ShmState&&) = delete;

  /// Release all resources (idempotent).
  void Reset() noexcept {
    bufs.at(0).buf.Reset();
    bufs.at(1).buf.Reset();
    bufs.at(0).pixels = nullptr;
    bufs.at(1).pixels = nullptr;
    if (data != MAP_FAILED) {
      munmap(data, total);
      data = MAP_FAILED;
      total = 0;
    }
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  /// Returns index of a non-busy buffer, or -1 when both are in use.
  [[nodiscard]] int NextFree() const noexcept {
    if (!bufs.at(0).buf.IsNull() && !bufs.at(0).buf.Get()->busy)
      return 0;
    if (!bufs.at(1).buf.IsNull() && !bufs.at(1).buf.Get()->busy)
      return 1;
    return -1;
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// Rendering helpers
// ══════════════════════════════════════════════════════════════════════════════

// Layout constants (pixels).
static constexpr int kMargin = 12;   // left/right/top margin
static constexpr int kCellW = 8;     // character cell width
static constexpr int kCellH = 16;    // character cell height
static constexpr int kFontPad = 4;   // padding above the 8-px glyph in a cell
static constexpr int kStatusH = 26;  // status-bar height at bottom

// Colors (0xXXRRGGBB — XRGB8888).
static constexpr uint32_t kColBg = 0xFFFFFFFF;        // text-area background
static constexpr uint32_t kColFg = 0xFF1E1E1E;        // normal text
static constexpr uint32_t kColCursorBg = 0xFF3584E4;  // cursor cell bg (blue)
static constexpr uint32_t kColCursorFg = 0xFFFFFFFF;  // cursor cell fg (white)
static constexpr uint32_t kColBarBg = 0xFF2E3440;     // status-bar background
static constexpr uint32_t kColBarFg = 0xFFD8DEE9;     // status-bar text

/// Fill a rectangle in the pixel buffer with a solid color.
static void FillRect(std::span<uint32_t> pixels,
                     int pitch,
                     int x,
                     int y,
                     int w,
                     int h,
                     uint32_t color) noexcept {
  for (int row = 0; row < h; ++row)
    for (int col = 0; col < w; ++col)
      pixels[static_cast<std::size_t>((y + row) * pitch + (x + col))] = color;
}

/// Render one 8×8 glyph at pixel position (x, y).
/// @p c must be a printable ASCII character (0x20–0x7E); others are skipped.
static void DrawGlyph(std::span<uint32_t> pixels,
                      int pitch,
                      int x,
                      int y,
                      char c,
                      uint32_t fg,
                      uint32_t bg) noexcept {
  const int idx = static_cast<unsigned char>(c) - 0x20;
  if (idx < 0 || idx >= 95)
    return;
  const uint8_t* glyph = kGlyph[idx];
  for (int row = 0; row < 8; ++row) {
    const uint8_t bits = glyph[row];
    for (int col = 0; col < 8; ++col) {
      const bool set = ((bits >> (7 - col)) & 1u) != 0u;
      pixels[static_cast<std::size_t>((y + row) * pitch + (x + col))] =
          set ? fg : bg;
    }
  }
}

/// Render a NUL-terminated string into the pixel buffer starting at (x, y).
/// Characters that would extend beyond @p max_x are not drawn.
static void DrawString(std::span<uint32_t> pixels,
                       int pitch,
                       int max_x,
                       int x,
                       int y,
                       const char* str,
                       uint32_t fg,
                       uint32_t bg) noexcept {
  for (; *str && x + kCellW <= max_x; ++str, x += kCellW)
    DrawGlyph(pixels, pitch, x, y, *str, fg, bg);
}

/// Render the full frame: text buffer, cursor, and status bar.
///
/// @param pixels  XRGB8888 pixel buffer, @p w × @p h pixels.
/// @param w       Buffer width in pixels (== stride / 4).
/// @param h       Buffer height in pixels.
/// @param buf     Text buffer to render.
static void RenderFrame(std::span<uint32_t> pixels,
                        int w,
                        int h,
                        const TextBuffer& buf) noexcept {
  const int pitch = w;

  // ── Fill background ────────────────────────────────────────────────────────
  FillRect(pixels, pitch, 0, 0, w, h, kColBg);

  // ── Compute text-grid dimensions ───────────────────────────────────────────
  const int cols = (w - 2 * kMargin) / kCellW;
  const int text_area_h = h - 2 * kMargin - kStatusH;
  const int rows = text_area_h / kCellH;
  if (cols <= 0 || rows <= 0)
    return;

  // ── First pass: determine the visual (col, row) of the cursor ─────────────
  // We need this to compute a vertical scroll offset so the cursor is always
  // within the visible rows (editor.c's text_entry_update_layout() equivalent).
  const std::string& text = buf.text();
  const std::size_t cursor = buf.cursor();

  int vcol = 0, vrow = 0;
  int cur_vrow = 0;
  bool cursor_found = false;

  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == cursor) {
      cur_vrow = vrow;
      cursor_found = true;
    }
    if (i == text.size())
      break;
    if (text[i] == '\n') {
      vcol = 0;
      ++vrow;
    } else {
      if (vcol >= cols) {
        vcol = 0;
        ++vrow;
      }
      ++vcol;
    }
  }
  if (!cursor_found) {
    cur_vrow = vrow;
  }

  // Vertical scroll: keep cursor in the visible row range.
  const int scroll = (cur_vrow >= rows) ? cur_vrow - rows + 1 : 0;

  // ── Second pass: render each character ────────────────────────────────────
  vcol = 0;
  vrow = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    const bool is_cursor = (i == cursor);
    // Render the cursor block even at end-of-text.
    if (i == text.size()) {
      if (is_cursor) {
        const int vr = vrow - scroll;
        if (vr >= 0 && vr < rows)
          FillRect(pixels, pitch, kMargin + vcol * kCellW,
                   kMargin + vr * kCellH, kCellW, kCellH, kColCursorBg);
      }
      break;
    }

    const char c = text[i];

    if (c == '\n') {
      // Render cursor block at this position (cursor on a newline character).
      if (is_cursor) {
        const int vr = vrow - scroll;
        if (vr >= 0 && vr < rows)
          FillRect(pixels, pitch, kMargin + vcol * kCellW,
                   kMargin + vr * kCellH, kCellW, kCellH, kColCursorBg);
      }
      vcol = 0;
      ++vrow;
      continue;
    }

    // Visual word-wrap at column boundary.
    if (vcol >= cols) {
      vcol = 0;
      ++vrow;
    }

    const int vr = vrow - scroll;
    if (vr >= 0 && vr < rows) {
      const int px = kMargin + vcol * kCellW;
      const int py = kMargin + vr * kCellH;
      if (is_cursor) {
        FillRect(pixels, pitch, px, py, kCellW, kCellH, kColCursorBg);
        DrawGlyph(pixels, pitch, px, py + kFontPad, c, kColCursorFg,
                  kColCursorBg);
      } else {
        DrawGlyph(pixels, pitch, px, py + kFontPad, c, kColFg, kColBg);
      }
    }
    ++vcol;
  }

  // ── Status bar ─────────────────────────────────────────────────────────────
  FillRect(pixels, pitch, 0, h - kStatusH, w, kStatusH, kColBarBg);

  const auto [ln, col_pos] = buf.CursorPos();
  char status[160];
  std::snprintf(status, sizeof(status),
                "  Ln %d, Col %d | %zu chars | arrows/home/end/bksp/del/enter "
                "repeatable | ESC: quit",
                ln + 1, col_pos + 1, text.size());
  DrawString(pixels, pitch, w, kMargin, h - kStatusH + (kStatusH - 8) / 2,
             status, kColBarFg, kColBarBg);
}

// ══════════════════════════════════════════════════════════════════════════════
// App class — mirrors simple-egl structure exactly
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks invoked by the CRTP handlers ──────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose();
  void OnKey(uint32_t key, uint32_t state);
  /// Called by wl::KeyboardHandler before OnKey for every key event that has
  /// an XKB keysym.  Used here to insert printable characters.
  void OnKeySym(xkb_keysym_t sym, uint32_t key, uint32_t state);

 private:
  // ── Member declaration order determines RAII destruction order.
  //    Declared first → destroyed last; declared last → destroyed first.
  //
  //    Destruction sequence (reverse of declaration order):
  //      seat_ (keyboard_ first, then seat_ inside) →
  //      xdg_toplevel_ → xdg_surface_ → xdg_wm_base_ →
  //      shm_state_ → shm_handler_ → surface_ → compositor_ →
  //      registry_ → display_

  // Wayland display — destroyed last.
  wl::DisplayHandle display_;

  // Registry — destroyed before display_.
  wl::CRegistry registry_;

  // wl_compositor — no protocol destroy request; wl_proxy_destroy on Reset().
  wl::WlPtr<WlCompositorHandler> compositor_;

  // wl_surface — CWlSurface::Destroy() sends wl_surface.destroy.
  wl::WlPtr<WlSurfaceHandler> surface_;

  // wl_shm — provides pixel-buffer format query and pool creation.
  wl::WlPtr<WlShmHandler> shm_handler_;

  // XDG CRTP handlers — destroyed in reverse: toplevel → surface → wm_base.
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  // Seat + keyboard manager — keyboard_ inside is destroyed before seat_.
  wl::SeatManager<App> seat_;

  // SHM double-buffer state — declared after seat_ so it is destroyed first,
  // releasing wl_buffer proxies before the wl_shm pool is gone.
  ShmState shm_state_;

  // ── Application state ────────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  int width_ = 640;
  int height_ = 400;

  // Text-editor state (models weston/editor.c's text_entry).
  TextBuffer text_buf_{"Type here and hold keys to test repeat.  ESC to quit."};

  // ── Globals recorded during registry scan ────────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  /// Maximum time (ms) to wait for a compositor response during startup.
  static constexpr int kRoundtripTimeoutMs = 5000;

  // ── Internal pipeline steps ─────────────────────────────────────────────
  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitShm();
  bool MainLoop();

  /// Render the current text buffer into a free SHM buffer and commit it.
  void Redraw() noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// Handler method implementations (need full App definition)
// ══════════════════════════════════════════════════════════════════════════════

// XDG handler methods are provided by wl::XdgSurfaceHandler<App> and
// wl::XdgToplevelHandler<App> from <wl/xdg_shell.hpp>.
// Seat/keyboard handling is provided by wl::SeatManager<App> from
// <wl/seat.hpp>.

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  // Send versioned seat/keyboard release requests before member destructors
  // run.  SeatManager::Release() calls wl_keyboard.release (v≥3) then
  // wl_seat.release (v≥5) before the WlPtr destructors fire.
  seat_.Release();
}

int App::Run() {
  if (!ConnectDisplay())
    return EXIT_FAILURE;
  if (!ScanGlobals())
    return EXIT_FAILURE;
  if (!BindGlobals())
    return EXIT_FAILURE;
  if (!CreateSurfaces())
    return EXIT_FAILURE;
  if (!InitShm())
    return EXIT_FAILURE;
  return MainLoop() ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ── ConnectDisplay
// ────────────────────────────────────────────────────────────

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "key-input: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

// ── ScanGlobals
// ───────────────────────────────────────────────────────────────

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "key-input: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using wl_comp = wayland::client::wl_compositor_traits;
    using wl_s = wayland::client::wl_shm_traits;
    using xdg_base = xdg_shell::client::xdg_wm_base_traits;
    using wl_seat = wayland::client::wl_seat_traits;

    if (iface == wl_comp::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
    } else if (iface == wl_s::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
    } else if (iface == xdg_base::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
    } else if (iface == wl_seat::interface_name) {
      seat_.Record(name, ver);
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "key-input: timed out waiting for global advertisements\n");
    return false;
  }
  if (!compositor_name_) {
    std::fprintf(stderr, "key-input: wl_compositor not advertised\n");
    return false;
  }
  if (!shm_name_) {
    std::fprintf(stderr, "key-input: wl_shm not advertised\n");
    return false;
  }
  if (!xdg_wm_base_name_) {
    std::fprintf(stderr, "key-input: xdg_wm_base not advertised\n");
    return false;
  }
  return true;
}

// ── BindGlobals
// ───────────────────────────────────────────────────────────────

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_compositor — no events; use Attach() rather than BindHandler().
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "key-input: wl_compositor bind failed\n");
    return false;
  }

  // wl_shm — receives wl_shm.format events listing supported pixel formats.
  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_handler_, shm_name_,
                                      shm_ver_)) {
    std::fprintf(stderr, "key-input: wl_shm bind failed\n");
    return false;
  }

  // xdg_wm_base — CRTP handler responds to ping automatically.
  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "key-input: xdg_wm_base bind failed\n");
    return false;
  }

  // wl_seat — optional; SeatManager::Bind() is a no-op if not advertised.
  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "key-input: wl_seat bind failed\n");
    return false;
  }

  // Roundtrip so wl_shm.format events and seat capabilities arrive.
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "key-input: timed out waiting for seat capabilities\n");
    return false;
  }

  // Verify XRGB8888 support (bit 1 in the format bitmask).
  constexpr uint32_t kXrgb8888Bit = 1u << WL_SHM_FORMAT_XRGB8888;
  if (!(shm_handler_.Get()->formats & kXrgb8888Bit)) {
    std::fprintf(stderr, "key-input: WL_SHM_FORMAT_XRGB8888 not supported\n");
    return false;
  }
  return true;
}

// ── CreateSurfaces
// ────────────────────────────────────────────────────────────

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  // wl_compositor.create_surface → wl_surface.
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "key-input: wl_compositor.create_surface failed\n");
    return false;
  }

  // xdg_wm_base.get_xdg_surface → xdg_surface.
  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "key-input: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  // xdg_surface.get_toplevel → xdg_toplevel.
  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "key-input: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;

  xdg_toplevel_.Get()->SetTitle("key-input — keyboard repeat demo");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.key-input");

  // Commit the empty surface to trigger the compositor's configure sequence.
  // The following roundtrip blocks until xdg_surface::configure fires, which
  // sets configured_ = true via OnXdgSurfaceConfigure().
  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "key-input: timed out waiting for xdg_surface configure\n");
    return false;
  }
  return true;
}

// ── InitShm
// ───────────────────────────────────────────────────────────────────
//
// Allocates an anonymous shared-memory file (memfd), maps it, creates a
// wl_shm_pool, and carves two wl_buffer objects from the pool.  The pool is
// destroyed immediately after; the buffers remain alive as long as ShmState.

bool App::InitShm() {
  using namespace wayland::client;

  shm_state_.Reset();  // safe to call on first run (all members are -1/null)

  const auto stride =
      static_cast<int32_t>(static_cast<std::size_t>(width_) * 4u);
  const auto buf_bytes =
      static_cast<std::size_t>(stride) * static_cast<std::size_t>(height_);
  const std::size_t total = buf_bytes * 2;

  shm_state_.fd = memfd_create("key-input", MFD_CLOEXEC);
  if (shm_state_.fd < 0) {
    std::fprintf(stderr, "key-input: memfd_create: %s\n", std::strerror(errno));
    return false;
  }
  if (ftruncate(shm_state_.fd, static_cast<off_t>(total)) < 0) {
    std::fprintf(stderr, "key-input: ftruncate: %s\n", std::strerror(errno));
    return false;
  }
  shm_state_.data = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED,
                         shm_state_.fd, 0);
  if (shm_state_.data == MAP_FAILED) {
    std::fprintf(stderr, "key-input: mmap: %s\n", std::strerror(errno));
    return false;
  }
  shm_state_.total = total;

  // Create the wl_shm_pool via the C API (like presentation-shm).  The pool is
  // only needed transiently to create the two buffers; we Reset() it below.
  wl::WlPtr<WlShmPoolHandler> pool;
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    wl_shm_pool* const raw_pool = wl_shm_create_pool(
        reinterpret_cast<wl_shm*>(shm_handler_.Get()->GetProxy()),
        shm_state_.fd, static_cast<int32_t>(total));
    if (!raw_pool) {
      std::fprintf(stderr, "key-input: wl_shm_create_pool failed\n");
      return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    pool.Attach(reinterpret_cast<wl_proxy*>(raw_pool));
  }

  for (int i = 0; i < 2; ++i) {
    const auto offset =
        static_cast<int32_t>(static_cast<std::size_t>(i) * buf_bytes);
    wl_proxy* const raw =
        wl::construct<wl_buffer_traits, wl_shm_pool_traits::Op::CreateBuffer>(
            *pool.Get(), offset, width_, height_, stride,
            WL_SHM_FORMAT_XRGB8888);
    if (!raw) {
      std::fprintf(stderr, "key-input: wl_shm_pool.create_buffer[%d] failed\n",
                   i);
      return false;
    }
    shm_state_.bufs.at(static_cast<std::size_t>(i)).buf.Get()->_SetProxy(raw);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    shm_state_.bufs.at(static_cast<std::size_t>(i)).pixels =
        static_cast<uint8_t*>(shm_state_.data) +
        static_cast<std::size_t>(i) * buf_bytes;
  }

  pool.Reset();  // wl_shm_pool no longer needed after buffer creation
  return true;
}

// ── MainLoop
// ──────────────────────────────────────────────────────────────────

bool App::MainLoop() {
  std::printf(
      "key-input: window ready\n"
      "  Type to edit.  Hold any key to observe keyboard repeat.\n"
      "  Backspace/Delete/arrows/Home/End/Enter all repeat.\n"
      "  ESC or close the window to quit.\n");

  // Render the initial frame before entering the event loop.
  Redraw();

  // Run the event loop with keyboard-repeat support.
  // get_repeat_fd() is re-evaluated each iteration: it returns -1 until the
  // compositor sends wl_keyboard::repeat_info (after seat capabilities arrive
  // and the keyboard is bound).  Once valid, the repeat pipe is polled
  // alongside the Wayland fd so held keys fire repeatedly via DispatchRepeat().
  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "key-input",
      [this] { return seat_.GetRepeatFd(); },
      [this] { seat_.DispatchRepeat(); });

  if (ok)
    std::printf("key-input: exiting cleanly\n");
  return ok;
}

// ── Redraw
// ────────────────────────────────────────────────────────────────────

void App::Redraw() noexcept {
  if (!configured_)
    return;

  const int idx = shm_state_.NextFree();
  if (idx < 0)
    return;  // both buffers still in use by the compositor — drop frame

  const std::size_t npixels =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* base = reinterpret_cast<uint32_t*>(
      shm_state_.bufs.at(static_cast<std::size_t>(idx)).pixels);
  RenderFrame({base, npixels}, width_, height_, text_buf_);

  surface_.Get()->Attach(
      shm_state_.bufs.at(static_cast<std::size_t>(idx)).buf.Get()->GetProxy(),
      0, 0);
  surface_.Get()->Damage(0, 0, width_, height_);
  surface_.Get()->Commit();
  shm_state_.bufs.at(static_cast<std::size_t>(idx)).buf.Get()->busy = true;
}

// ── App callbacks
// ─────────────────────────────────────────────────────────────

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  configured_ = true;
}

void App::OnToplevelConfigure(const int32_t w, const int32_t h) {
  // Clamp to a sane upper bound to guard against malicious compositor values.
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    const int new_w = std::min(w, kMaxDim);
    const int new_h = std::min(h, kMaxDim);
    if (new_w != width_ || new_h != height_) {
      width_ = new_w;
      height_ = new_h;
      // Re-allocate SHM buffers at the new size.  InitShm() calls Reset()
      // internally before allocating, so this is safe to call repeatedly.
      if (!InitShm()) {
        std::fprintf(stderr, "key-input: SHM resize failed\n");
        running_ = false;
      }
    }
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

// ── OnKey — evdev key-code handler (control keys + repeat) ───────────────────
//
// Called for every key press and release, and also on each keyboard-repeat
// tick (via wl::KeyboardHandler::DispatchRepeat → SeatManager::DispatchRepeat
// → App::OnKey with WL_KEYBOARD_KEY_STATE_PRESSED).
//
// Only WL_KEYBOARD_KEY_STATE_PRESSED events are acted upon; releases are
// ignored.  Because repeat fires with PRESSED state, holding any of the keys
// below produces continuous effect — exactly like weston's editor.c.

void App::OnKey(const uint32_t key, const uint32_t state) {
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;

  bool dirty = false;
  switch (key) {
    case KEY_BACKSPACE:
      text_buf_.Backspace();
      dirty = true;
      break;
    case KEY_DELETE:
      text_buf_.DeleteForward();
      dirty = true;
      break;
    case KEY_LEFT:
      text_buf_.MoveLeft();
      dirty = true;
      break;
    case KEY_RIGHT:
      text_buf_.MoveRight();
      dirty = true;
      break;
    case KEY_UP:
      text_buf_.MoveUp();
      dirty = true;
      break;
    case KEY_DOWN:
      text_buf_.MoveDown();
      dirty = true;
      break;
    case KEY_HOME:
      text_buf_.MoveHome();
      dirty = true;
      break;
    case KEY_END:
      text_buf_.MoveEnd();
      dirty = true;
      break;
    case KEY_ENTER:
    case KEY_KPENTER:
      text_buf_.Insert('\n');
      dirty = true;
      break;
    case KEY_ESC:
      running_ = false;
      break;
    default:
      break;
  }

  if (dirty)
    Redraw();
}

// ── OnKeySym — XKB keysym handler (printable character insertion)
// ─────────────
//
// Called before OnKey for every key event that has an XKB keysym.  Used to
// insert printable ASCII characters (models text_model_keysym() in editor.c).
// Non-printable keysyms (arrows, function keys, modifiers, …) produce a
// utf32 value of 0 or outside 0x20–0x7E and are silently skipped.
//
// Keyboard repeat fires OnKeySym first (via KeyboardHandler::DispatchRepeat),
// so holding a printable key inserts it repeatedly — the key repeat feature
// being demonstrated.

void App::OnKeySym(const xkb_keysym_t sym,
                   const uint32_t /*key*/,
                   const uint32_t state) {
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;

  // Convert XKB keysym to Unicode code point.
  const uint32_t cp = xkb_keysym_to_utf32(sym);

  // Only insert printable ASCII; the inline font covers exactly 0x20–0x7E.
  if (cp < 0x20u || cp > 0x7Eu)
    return;

  text_buf_.Insert(static_cast<char>(cp));
  Redraw();
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  // Suppress SIGPIPE so that a compositor disconnect during wl_display_flush
  // is reported as EPIPE / error return rather than terminating the process.
  std::signal(SIGPIPE, SIG_IGN);

  App app;
  return app.Run();
}
