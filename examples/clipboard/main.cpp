// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// clipboard — wl_data_device selection (clipboard) demonstration
//
// A mapped xdg_toplevel that both reads and writes the core Wayland clipboard
// via wl::DataDevice<App>:
//
//   Copy  — type a line of text and press Enter.  The Enter key's input serial
//           (wl::KeyEvent::serial) is handed to wl_data_device.set_selection,
//           which the compositor requires to come from a real input event.
//           When another client pastes, wl::DataDevice calls App::OnSend and we
//           write the text into the compositor-provided pipe.
//   Paste — when another client takes the selection, wl::DataDevice calls
//           App::OnSelection with the offered MIME types; we ask for a text
//           flavor via Receive() and show the payload.
//
// Because the core wl_data_device selection is delivered only to the client
// with keyboard focus (and set_selection needs a focused input serial), this is
// a windowed demo rather than a headless CLI — a surface with keyboard focus is
// exactly what the protocol requires.  ext-data-control lifts that restriction;
// wl::DataDevice drives it from a different protocol-traits bundle.
//
// Rendering uses wl_shm (XRGB8888) with an inline 8x8 bitmap font; no
// EGL/Mesa/GPU dependency.  App structure mirrors the other windowed examples:
//   ConnectDisplay -> ScanGlobals -> BindGlobals -> CreateSurfaces ->
//   InitShm -> MainLoop
//
// Build requirements: wayland-client, wayland-protocols, xkbcommon.
// Runtime requirement: a running Wayland compositor with xdg-shell support.

// ── Generated C++ protocol headers ───────────────────────────────────────────
// wayland_client.hpp   -> namespace wayland::client  (from wayland.xml)
// xdg_shell_client.hpp -> namespace xdg_shell::client (from xdg-shell.xml)
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>
#include <wl/data_device.hpp>
#include <wl/display.hpp>
#include <wl/fd_handle.hpp>
#include <wl/registry.hpp>
#include <wl/seat.hpp>
#include <wl/span.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

// ── System headers
// ────────────────────────────────────────────────────────────
extern "C" {
#include <linux/input-event-codes.h>  // KEY_ESC, KEY_ENTER, KEY_BACKSPACE, …
#include <poll.h>                     // poll (bounded paste read)
#include <sys/mman.h>                 // memfd_create, mmap, munmap
#include <unistd.h>                   // close, ftruncate, read, write
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>  // xkb_keysym_to_utf32
}

// ── Standard library
// ──────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// The traits generated from wayland.xml declare wl_iface() but leave it for the
// consumer to bind to the libwayland interface symbol (from
// wayland-client-protocol.h).  wl_seat_traits::wl_iface() is provided inline by
// <wl/seat.hpp>; all xdg_shell traits by <wl/xdg_shell.hpp>.
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
const wl_interface& wl_data_device_manager_traits::wl_iface() noexcept {
  return wl_data_device_manager_interface;
}
const wl_interface& wl_data_device_traits::wl_iface() noexcept {
  return wl_data_device_interface;
}
const wl_interface& wl_data_offer_traits::wl_iface() noexcept {
  return wl_data_offer_interface;
}
const wl_interface& wl_data_source_traits::wl_iface() noexcept {
  return wl_data_source_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// Inline 8x8 bitmap font — printable ASCII (0x20 … 0x7E, 95 glyphs)
//
// Each entry is 8 bytes, one per row (top to bottom); within each byte bit 7 is
// the leftmost pixel.  The glyph occupies a 6-pixel-wide column, leaving
// 1-pixel margins on each side.
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
// Rendering helpers
// ══════════════════════════════════════════════════════════════════════════════

static constexpr int kMargin = 12;   // left/top margin
static constexpr int kCellW = 8;     // character cell width
static constexpr int kLineH = 18;    // line height
static constexpr int kStatusH = 26;  // status-bar height at bottom

// Colors (0xXXRRGGBB — XRGB8888).
static constexpr uint32_t kColBg = 0xFFFFFFFF;      // background
static constexpr uint32_t kColFg = 0xFF1E1E1E;      // normal text
static constexpr uint32_t kColDim = 0xFF6A737D;     // labels / hints
static constexpr uint32_t kColOwn = 0xFF2E7D32;     // "owned" accent (green)
static constexpr uint32_t kColCursor = 0xFF3584E4;  // edit cursor (blue)
static constexpr uint32_t kColBarBg = 0xFF2E3440;   // status-bar background
static constexpr uint32_t kColBarFg = 0xFFD8DEE9;   // status-bar text

static void FillRect(wl::span<uint32_t> pixels,
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

static void DrawGlyph(wl::span<uint32_t> pixels,
                      int pitch,
                      int x,
                      int y,
                      char c,
                      uint32_t fg) noexcept {
  const int idx = static_cast<unsigned char>(c) - 0x20;
  if (idx < 0 || idx >= 95)
    return;
  const uint8_t* glyph = kGlyph[idx];
  for (int row = 0; row < 8; ++row) {
    const uint8_t bits = glyph[row];
    for (int col = 0; col < 8; ++col)
      if (((bits >> (7 - col)) & 1u) != 0u)
        pixels[static_cast<std::size_t>((y + row) * pitch + (x + col))] = fg;
  }
}

// Draw a string starting at (x, y); characters past @p max_x are clipped.
// Returns the x just past the last drawn glyph (for cursor placement).
static int DrawString(wl::span<uint32_t> pixels,
                      int pitch,
                      int max_x,
                      int x,
                      int y,
                      std::string_view str,
                      uint32_t fg) noexcept {
  for (const char c : str) {
    if (x + kCellW > max_x)
      break;
    DrawGlyph(pixels, pitch, x, y, c, fg);
    x += kCellW;
  }
  return x;
}

// ══════════════════════════════════════════════════════════════════════════════
// CRTP handler classes
// ══════════════════════════════════════════════════════════════════════════════

class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};

class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

class WlShmPoolHandler : public wayland::client::CWlShmPool<WlShmPoolHandler> {
};

class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {
 public:
  uint32_t formats = 0;
  void OnFormat(uint32_t fmt) override {
    if (fmt < 32u)
      formats |= (1u << fmt);
  }
};

class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool busy = false;
  void OnRelease() override { busy = false; }
};

// ══════════════════════════════════════════════════════════════════════════════
// SHM double-buffer state (one shared pool, two XRGB8888 buffers)
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

  [[nodiscard]] int NextFree() const noexcept {
    if (!bufs.at(0).buf.IsNull() && !bufs.at(0).buf.Get()->busy)
      return 0;
    if (!bufs.at(1).buf.IsNull() && !bufs.at(1).buf.Get()->busy)
      return 1;
    return -1;
  }
};

// ══════════════════════════════════════════════════════════════════════════════
// App — clipboard demo
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  int Run();
  ~App();

  // ── Callbacks invoked by the CRTP handlers ──────────────────────────────
  void OnXdgSurfaceConfigure(uint32_t serial);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose();
  void OnKey(const wl::KeyEvent& ev);

  // ── wl::DataDevice<App> hooks (detected via SFINAE) ─────────────────────
  void OnSelection(const wl::MimeSet& mimes);      // an external selection
  void OnSend(const char* mime, wl::FdHandle fd);  // a peer is pasting ours
  void OnCancelled();                              // our selection was dropped

 private:
  // Member order fixes RAII teardown (declared first → destroyed last).
  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<WlShmHandler> shm_handler_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;
  wl::SeatManager<App> seat_;
  wl::DataDevice<App> data_device_;  // core wl_data_device clipboard helper
  ShmState shm_state_;

  // ── Application state ────────────────────────────────────────────────────
  bool running_ = true;
  bool configured_ = false;
  int width_ = 640;
  int height_ = 320;

  std::string edit_;            // text being composed for copy
  std::string pasted_;          // last payload read from a peer's selection
  std::string peer_mimes_;      // comma-joined MIME types of the peer offer
  bool own_selection_ = false;  // this window currently owns the clipboard
  bool have_manager_ = false;   // wl_data_device_manager was advertised
  std::string status_ = "Ready.";

  // ── Globals recorded during registry scan ────────────────────────────────
  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;

  static constexpr int kRoundtripTimeoutMs = 5000;
  // Bound on how long a synchronous paste read may block the event loop when a
  // peer is slow to write; a well-behaved source answers in microseconds.
  static constexpr int kPasteReadTimeoutMs = 500;

  bool ConnectDisplay();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurfaces();
  bool InitShm();
  bool MainLoop();
  void Redraw() noexcept;

  // Publish edit_ as the selection using @p serial from a real key event.
  void CopyCurrent(uint32_t serial);
  // Pick the best text flavor from an offer, or nullptr if none is text.
  static const char* PickTextMime(const wl::MimeSet& mimes) noexcept;
};

// ══════════════════════════════════════════════════════════════════════════════
// App method implementations
// ══════════════════════════════════════════════════════════════════════════════

App::~App() {
  // Tear down clipboard proxies and the seat before the WlPtr/display
  // destructors run.  DataDevice::Release() sends the versioned data-device
  // release; SeatManager::Release() the keyboard/seat releases.
  data_device_.Release();
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

bool App::ConnectDisplay() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "clipboard: wl_display_connect: %s\n",
                 std::strerror(errno));
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "clipboard: wl_display_get_registry failed\n");
    return false;
  }

  registry_.OnGlobal([this](wl::CRegistry& /*reg*/, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using wl_comp = wayland::client::wl_compositor_traits;
    using wl_s = wayland::client::wl_shm_traits;
    using xdg_base = xdg_shell::client::xdg_wm_base_traits;
    using wl_seat = wayland::client::wl_seat_traits;
    using wl_ddm = wayland::client::wl_data_device_manager_traits;

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
    } else if (iface == wl_ddm::interface_name) {
      data_device_.Record(name, ver);
      have_manager_ = true;
    }
  });

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "clipboard: timed out waiting for global advertisements\n");
    return false;
  }
  if (!compositor_name_ || !shm_name_ || !xdg_wm_base_name_) {
    std::fprintf(stderr,
                 "clipboard: compositor/shm/xdg_wm_base not advertised\n");
    return false;
  }
  return true;
}

bool App::BindGlobals() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version))) {
    compositor_.Attach(raw);
  } else {
    std::fprintf(stderr, "clipboard: wl_compositor bind failed\n");
    return false;
  }

  if (!wl::BindHandler<wl_shm_traits>(registry_, shm_handler_, shm_name_,
                                      shm_ver_)) {
    std::fprintf(stderr, "clipboard: wl_shm bind failed\n");
    return false;
  }

  if (!wl::BindHandler<xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "clipboard: xdg_wm_base bind failed\n");
    return false;
  }

  if (!seat_.Bind(registry_, this)) {
    std::fprintf(stderr, "clipboard: wl_seat bind failed\n");
    return false;
  }

  // wl_data_device_manager — optional; Bind() is a no-op if not advertised.
  if (!data_device_.Bind(registry_, this)) {
    std::fprintf(stderr, "clipboard: wl_data_device_manager bind failed\n");
    return false;
  }

  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "clipboard: timed out waiting for seat capabilities\n");
    return false;
  }

  constexpr uint32_t kXrgb8888Bit = 1u << WL_SHM_FORMAT_XRGB8888;
  if (!(shm_handler_.Get()->formats & kXrgb8888Bit)) {
    std::fprintf(stderr, "clipboard: WL_SHM_FORMAT_XRGB8888 not supported\n");
    return false;
  }

  // Now that the seat proxy exists, wire the data device to it so selection
  // events (paste) start flowing and Offer() (copy) has a device to act on.
  wl_proxy* seat = seat_.Seat();
  if (!have_manager_) {
    status_ = "No wl_data_device_manager: clipboard unavailable.";
  } else if (seat == nullptr) {
    status_ = "No seat: clipboard needs a keyboard for focus.";
  } else {
    data_device_.Start(display_.Get(), seat);
  }
  return true;
}

bool App::CreateSurfaces() {
  using namespace wayland::client;
  using namespace xdg_shell::client;

  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get())) {
    surface_.Get()->_SetProxy(raw);
  } else {
    std::fprintf(stderr, "clipboard: wl_compositor.create_surface failed\n");
    return false;
  }

  if (!wl::SetupHandler(xdg_surface_,
                        wl::construct<xdg_surface_traits,
                                      xdg_wm_base_traits::Op::GetXdgSurface>(
                            *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "clipboard: xdg_wm_base.get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(xdg_toplevel_,
                        wl::construct<xdg_toplevel_traits,
                                      xdg_surface_traits::Op::GetToplevel>(
                            *xdg_surface_.Get()))) {
    std::fprintf(stderr, "clipboard: xdg_surface.get_toplevel failed\n");
    return false;
  }
  xdg_toplevel_.Get()->app_ = this;
  xdg_toplevel_.Get()->SetTitle("clipboard — wl_data_device demo");
  xdg_toplevel_.Get()->SetAppId("org.wayland-cxx.clipboard");

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get())) {
    std::fprintf(stderr,
                 "clipboard: timed out waiting for xdg_surface configure\n");
    return false;
  }
  return true;
}

bool App::InitShm() {
  using namespace wayland::client;

  shm_state_.Reset();

  const auto stride =
      static_cast<int32_t>(static_cast<std::size_t>(width_) * 4u);
  const auto buf_bytes =
      static_cast<std::size_t>(stride) * static_cast<std::size_t>(height_);
  const std::size_t total = buf_bytes * 2;

  shm_state_.fd = memfd_create("clipboard", MFD_CLOEXEC);
  if (shm_state_.fd < 0) {
    std::fprintf(stderr, "clipboard: memfd_create: %s\n", std::strerror(errno));
    return false;
  }
  if (ftruncate(shm_state_.fd, static_cast<off_t>(total)) < 0) {
    std::fprintf(stderr, "clipboard: ftruncate: %s\n", std::strerror(errno));
    return false;
  }
  shm_state_.data = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED,
                         shm_state_.fd, 0);
  if (shm_state_.data == MAP_FAILED) {
    std::fprintf(stderr, "clipboard: mmap: %s\n", std::strerror(errno));
    return false;
  }
  shm_state_.total = total;

  wl::WlPtr<WlShmPoolHandler> pool;
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    wl_shm_pool* const raw_pool = wl_shm_create_pool(
        reinterpret_cast<wl_shm*>(shm_handler_.Get()->GetProxy()),
        shm_state_.fd, static_cast<int32_t>(total));
    if (!raw_pool) {
      std::fprintf(stderr, "clipboard: wl_shm_create_pool failed\n");
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
      std::fprintf(stderr, "clipboard: wl_shm_pool.create_buffer[%d] failed\n",
                   i);
      return false;
    }
    shm_state_.bufs.at(static_cast<std::size_t>(i)).buf.Get()->_SetProxy(raw);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    shm_state_.bufs.at(static_cast<std::size_t>(i)).pixels =
        static_cast<uint8_t*>(shm_state_.data) +
        static_cast<std::size_t>(i) * buf_bytes;
  }

  pool.Reset();
  return true;
}

bool App::MainLoop() {
  std::printf(
      "clipboard: window ready\n"
      "  Type a line of text, press Enter to copy it to the clipboard.\n"
      "  Paste into another app to pull it; copy in another app to see it "
      "here.\n"
      "  Backspace edits, ESC or the close button quits.\n");

  Redraw();

  const bool ok = wl::RunEventLoop(
      display_.Get(), [this] { return !running_; }, "clipboard",
      [this] { return seat_.GetRepeatFd(); },
      [this] { seat_.DispatchRepeat(); });

  if (ok)
    std::printf("clipboard: exiting cleanly\n");
  return ok;
}

void App::Redraw() noexcept {
  if (!configured_)
    return;

  const int idx = shm_state_.NextFree();
  if (idx < 0)
    return;

  const std::size_t npixels =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* base = reinterpret_cast<uint32_t*>(
      shm_state_.bufs.at(static_cast<std::size_t>(idx)).pixels);
  wl::span<uint32_t> px{base, npixels};
  const int pitch = width_;
  const int max_x = width_ - kMargin;

  FillRect(px, pitch, 0, 0, width_, height_, kColBg);

  int y = kMargin;
  DrawString(px, pitch, max_x, kMargin, y, "wayland-cxx clipboard demo",
             kColFg);
  y += kLineH + kLineH / 2;

  // ── Copy (edit) row ──────────────────────────────────────────────────────
  DrawString(px, pitch, max_x, kMargin, y, "Copy (Enter):", kColDim);
  y += kLineH;
  const int ex =
      DrawString(px, pitch, max_x, kMargin + kCellW, y, edit_, kColFg);
  // Blinking-free caret block after the edit text.
  if (ex + kCellW <= max_x)
    FillRect(px, pitch, ex, y - 1, 2, 10, kColCursor);
  y += kLineH + kLineH / 2;

  // ── Paste (current selection) rows ───────────────────────────────────────
  DrawString(px, pitch, max_x, kMargin, y, "Clipboard now:", kColDim);
  y += kLineH;
  if (own_selection_) {
    DrawString(px, pitch, max_x, kMargin + kCellW, y,
               "(owned by this window) " + edit_, kColOwn);
    y += kLineH;
  } else if (!peer_mimes_.empty()) {
    DrawString(px, pitch, max_x, kMargin + kCellW, y, "types: " + peer_mimes_,
               kColDim);
    y += kLineH;
    DrawString(px, pitch, max_x, kMargin + kCellW, y, "text:  " + pasted_,
               kColFg);
    y += kLineH;
  } else {
    DrawString(px, pitch, max_x, kMargin + kCellW, y, "(empty)", kColDim);
    y += kLineH;
  }

  // ── Status bar ───────────────────────────────────────────────────────────
  FillRect(px, pitch, 0, height_ - kStatusH, width_, kStatusH, kColBarBg);
  DrawString(px, pitch, max_x, kMargin, height_ - kStatusH + (kStatusH - 8) / 2,
             "  " + status_, kColBarFg);

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
  static constexpr int32_t kMaxDim = 16384;
  if (w > 0 && h > 0) {
    const int new_w = std::min(w, kMaxDim);
    const int new_h = std::min(h, kMaxDim);
    if (new_w != width_ || new_h != height_) {
      width_ = new_w;
      height_ = new_h;
      if (!InitShm()) {
        std::fprintf(stderr, "clipboard: SHM resize failed\n");
        running_ = false;
      }
    }
  }
}

void App::OnToplevelClose() {
  running_ = false;
}

void App::OnKey(const wl::KeyEvent& ev) {
  if (ev.state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;

  switch (ev.key) {
    case KEY_ESC:
      running_ = false;
      return;
    case KEY_BACKSPACE:
      if (!edit_.empty()) {
        edit_.pop_back();
        Redraw();
      }
      return;
    case KEY_ENTER:
    case KEY_KPENTER:
      // The compositor requires set_selection to carry the serial of a real
      // input event; ev.serial is exactly that (from wl_keyboard.key).
      CopyCurrent(ev.serial);
      return;
    default:
      break;
  }

  const uint32_t cp = xkb_keysym_to_utf32(ev.keysym);
  if (cp < 0x20u || cp > 0x7Eu)
    return;
  edit_.push_back(static_cast<char>(cp));
  Redraw();
}

void App::CopyCurrent(uint32_t serial) {
  if (edit_.empty()) {
    status_ = "Nothing to copy — type some text first.";
    Redraw();
    return;
  }
  wl::MimeSet mimes;
  mimes.Add("text/plain;charset=utf-8");
  mimes.Add("text/plain");
  mimes.Add("UTF8_STRING");
  mimes.Add("STRING");
  mimes.Add("TEXT");
  data_device_.Offer(mimes, serial);
  own_selection_ = true;
  peer_mimes_.clear();
  pasted_.clear();
  status_ = "Copied " + std::to_string(edit_.size()) + " bytes to clipboard.";
  std::printf("clipboard: copied %zu bytes: \"%s\"\n", edit_.size(),
              edit_.c_str());
  std::fflush(stdout);
  Redraw();
}

const char* App::PickTextMime(const wl::MimeSet& mimes) noexcept {
  for (const char* m : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                        "STRING", "TEXT"})
    if (mimes.Contains(m))
      return m;
  return nullptr;
}

void App::OnSelection(const wl::MimeSet& mimes) {
  // A new selection is current.  If we own it, the offer we would receive is
  // served by our own OnSend on this same thread — reading it synchronously
  // would deadlock — so just reflect ownership and skip the read.
  if (own_selection_) {
    Redraw();
    return;
  }

  peer_mimes_.clear();
  for (const std::string& m : mimes) {
    if (!peer_mimes_.empty())
      peer_mimes_ += ", ";
    peer_mimes_ += m;
  }
  pasted_.clear();

  const char* mime = PickTextMime(mimes);
  if (mime == nullptr) {
    status_ = mimes.empty() ? "Selection cleared." : "Selection has no text.";
    Redraw();
    return;
  }

  wl::FdHandle fd = data_device_.Receive(mime);
  if (fd.Get() < 0) {
    status_ = "Receive failed.";
    Redraw();
    return;
  }

  // Bounded read: a well-behaved peer answers immediately, but guard against a
  // slow/stuck source so the event loop is never blocked indefinitely.
  std::array<char, 4096> buf{};
  for (;;) {
    pollfd pfd{fd.Get(), POLLIN, 0};
    const int pr = poll(&pfd, 1, kPasteReadTimeoutMs);
    if (pr <= 0)
      break;  // timeout or error
    const ssize_t n = read(fd.Get(), buf.data(), buf.size());
    if (n <= 0)
      break;  // EOF or error
    pasted_.append(buf.data(), static_cast<std::size_t>(n));
    if (pasted_.size() > (1u << 20))
      break;  // cap the preview at 1 MiB
  }

  status_ =
      "Pasted " + std::to_string(pasted_.size()) + " bytes (" + mime + ").";
  std::printf("clipboard: selection [%s] -> read %zu bytes as %s: \"%.*s\"\n",
              peer_mimes_.c_str(), pasted_.size(), mime,
              static_cast<int>(std::min<std::size_t>(pasted_.size(), 80)),
              pasted_.c_str());
  std::fflush(stdout);
  Redraw();
}

void App::OnSend(const char* /*mime*/, wl::FdHandle fd) {
  // A peer is pasting our selection: write the composed text into the pipe.
  // fd closes when this FdHandle goes out of scope, giving the reader EOF.
  const char* p = edit_.data();
  std::size_t left = edit_.size();
  while (left > 0) {
    const ssize_t n = write(fd.Get(), p, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;  // peer closed early (EPIPE) or other error
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  std::printf("clipboard: served %zu bytes to a pasting peer\n", edit_.size());
  std::fflush(stdout);
}

void App::OnCancelled() {
  own_selection_ = false;
  status_ = "Selection superseded by another client.";
  std::printf("clipboard: our selection was superseded\n");
  std::fflush(stdout);
  Redraw();
}

// ══════════════════════════════════════════════════════════════════════════════
// Entry point
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  // A peer that closes the paste pipe early would otherwise deliver SIGPIPE
  // during OnSend's write(); treat it as EPIPE instead.
  std::signal(SIGPIPE, SIG_IGN);

  App app;
  return app.Run();
}
