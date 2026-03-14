// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// wayland-info — print information about the Wayland compositor's globals.
//
// Connects to the running compositor via WAYLAND_DISPLAY, enumerates all
// globals advertised by wl_registry, and for well-known interfaces binds and
// prints detailed properties:
//
//   wl_shm               → supported pixel formats
//   wl_seat              → capabilities (pointer / keyboard / touch) and name
//   wl_output            → geometry, modes, and scale
//   zxdg_output_manager_v1  (requires wayland-protocols)
//                        → logical position and size per output
//   zwp_linux_dmabuf_v1  (requires wayland-protocols)
//                        → DRM pixel-format and modifier table
//
// Reference:
//   https://gitlab.freedesktop.org/wayland/wayland-utils/-/blob/main/wayland-info/wayland-info.c

// ── Generated C++ protocol headers ───────────────────────────────────────────
// wayland_client.hpp → namespace wayland::client  (from wayland.xml, always)
#include "wayland_client.hpp"

#if defined(HAVE_XDG_OUTPUT)
// xdg_output_client.hpp → namespace xdg_output_unstable_v1::client
#include "xdg_output_client.hpp"
#endif

#if defined(HAVE_LINUX_DMABUF)
// linux_dmabuf_client.hpp → namespace linux_dmabuf_unstable_v1::client
#include "linux_dmabuf_client.hpp"
#endif

// ── System Wayland C header ───────────────────────────────────────────────────
extern "C" {
// Pre-built wl_interface symbols for every core Wayland interface.
#include <wayland-client-protocol.h>
}

// ── Framework headers ─────────────────────────────────────────────────────────
#include <wl/proxy_impl.hpp>  // wl::construct<>
#include <wl/registry.hpp>    // wl::CRegistry
#include <wl/wl_ptr.hpp>      // wl::WlPtr<>

// ── Standard library ──────────────────────────────────────────────────────────
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>  // std::data
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces
//
// wayland-client-protocol.h provides extern const wl_interface symbols for
// every core Wayland interface.  We simply forward-declare them here.
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

const wl_interface& wl_shm_traits::wl_iface() noexcept {
  return wl_shm_interface;
}
const wl_interface& wl_seat_traits::wl_iface() noexcept {
  return wl_seat_interface;
}
const wl_interface& wl_output_traits::wl_iface() noexcept {
  return wl_output_interface;
}

}  // namespace wayland::client

// ══════════════════════════════════════════════════════════════════════════════
// wl_interface definitions — xdg-output-unstable-v1
//
// There are no pre-built system symbols for the xdg-output interfaces (unlike
// core Wayland).  We reproduce the exact same tables that the C
// wayland-scanner generates from xdg-output-unstable-v1.xml (version 3).
//
// Layout mirrors what the reference C scanner emits for this protocol:
//   • xdg_output_types[]   shared pointer array (types fields point into it)
//   • per-interface wl_message arrays (requests / events)
//   • wl_interface object definitions
// ══════════════════════════════════════════════════════════════════════════════

#if defined(HAVE_XDG_OUTPUT)

// Forward declarations — the types array references both interface objects
// before their full definitions appear below.
extern const wl_interface zxdg_output_manager_v1_iface_def;
extern const wl_interface zxdg_output_v1_iface_def;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* xdg_output_types[] = {
    nullptr,                      // [0]  scalar / no-type slots
    nullptr,                      // [1]
    nullptr,                      // [2]
    &zxdg_output_v1_iface_def,   // [3]  get_xdg_output → new_id
    &wl_output_interface,         // [4]  get_xdg_output → output object
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// Scalar-only types pointer (re-used by every event that has no object args).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static constexpr const wl_interface** kXdgOutputScalars = &xdg_output_types[0];

// ── zxdg_output_manager_v1 ───────────────────────────────────────────────────
static constexpr wl_message zxdg_output_manager_v1_requests[] = {
    {"destroy",        "",   nullptr},               // opcode 0 (destructor)
    {"get_xdg_output", "no", &xdg_output_types[3]}, // opcode 1
};
// (no events on the manager)

// ── zxdg_output_v1 ───────────────────────────────────────────────────────────
static constexpr wl_message zxdg_output_v1_requests[] = {
    {"destroy", "", nullptr},  // opcode 0 (destructor)
};
static constexpr wl_message zxdg_output_v1_events[] = {
    {"logical_position", "ii",  kXdgOutputScalars},  // opcode 0
    {"logical_size",     "ii",  kXdgOutputScalars},  // opcode 1
    {"done",             "",    nullptr},              // opcode 2
    {"name",             "2s",  kXdgOutputScalars},   // opcode 3 (since v2)
    {"description",      "2s",  kXdgOutputScalars},   // opcode 4 (since v2)
};

// clang-format off
const wl_interface zxdg_output_manager_v1_iface_def = {
    "zxdg_output_manager_v1", 3,
    2, std::data(zxdg_output_manager_v1_requests),
    0, nullptr};
const wl_interface zxdg_output_v1_iface_def = {
    "zxdg_output_v1", 3,
    1, std::data(zxdg_output_v1_requests),
    5, std::data(zxdg_output_v1_events)};
// clang-format on

namespace xdg_output_unstable_v1::client {

const wl_interface& zxdg_output_manager_v1_traits::wl_iface() noexcept {
  return zxdg_output_manager_v1_iface_def;
}
const wl_interface& zxdg_output_v1_traits::wl_iface() noexcept {
  return zxdg_output_v1_iface_def;
}

}  // namespace xdg_output_unstable_v1::client

#endif  // HAVE_XDG_OUTPUT

// ══════════════════════════════════════════════════════════════════════════════
// wl_interface definitions — linux-dmabuf-unstable-v1
//
// We expose version 3 of zwp_linux_dmabuf_v1 (format + modifier events).
// create_params references zwp_linux_buffer_params_v1; a minimal stub is
// provided because only its name is used by wl_registry_bind / wl_proxy.
// ══════════════════════════════════════════════════════════════════════════════

#if defined(HAVE_LINUX_DMABUF)

extern const wl_interface zwp_linux_dmabuf_v1_iface_def;
extern const wl_interface zwp_linux_buffer_params_v1_iface_def;

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,
//             cppcoreguidelines-avoid-non-const-global-variables,
//             cppcoreguidelines-interfaces-global-init)
static const wl_interface* linux_dmabuf_types[] = {
    nullptr,                                  // [0]  scalar slot
    &zwp_linux_buffer_params_v1_iface_def,    // [1]  create_params → new_id
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,
//           cppcoreguidelines-avoid-non-const-global-variables,
//           cppcoreguidelines-interfaces-global-init)

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static constexpr const wl_interface** kDmabufScalars = &linux_dmabuf_types[0];

static constexpr wl_message zwp_linux_dmabuf_v1_requests[] = {
    {"destroy",       "",  nullptr},                    // opcode 0 (destructor)
    {"create_params", "n", &linux_dmabuf_types[1]},     // opcode 1
};
static constexpr wl_message zwp_linux_dmabuf_v1_events[] = {
    {"format",   "u",    kDmabufScalars},  // opcode 0 (v1)
    {"modifier", "3uuu", kDmabufScalars},  // opcode 1 (v3)
};

// Minimal stub — only the interface name is used for proxy creation.
// clang-format off
const wl_interface zwp_linux_buffer_params_v1_iface_def = {
    "zwp_linux_buffer_params_v1", 5,
    0, nullptr, 0, nullptr};
const wl_interface zwp_linux_dmabuf_v1_iface_def = {
    "zwp_linux_dmabuf_v1", 3,
    2, std::data(zwp_linux_dmabuf_v1_requests),
    2, std::data(zwp_linux_dmabuf_v1_events)};
// clang-format on

namespace linux_dmabuf_unstable_v1::client {

const wl_interface& zwp_linux_dmabuf_v1_traits::wl_iface() noexcept {
  return zwp_linux_dmabuf_v1_iface_def;
}

}  // namespace linux_dmabuf_unstable_v1::client

#endif  // HAVE_LINUX_DMABUF

// ══════════════════════════════════════════════════════════════════════════════
// Data types
// ══════════════════════════════════════════════════════════════════════════════

struct GlobalInfo {
  uint32_t name;
  std::string interface;
  uint32_t version;
};

struct OutputMode {
  uint32_t flags;
  int32_t width;
  int32_t height;
  int32_t refresh;
};

// ══════════════════════════════════════════════════════════════════════════════
// CRTP event-handler classes
// ══════════════════════════════════════════════════════════════════════════════

// ── wl_shm ───────────────────────────────────────────────────────────────────

class WlShmInfo : public wayland::client::CWlShm<WlShmInfo> {
 public:
  std::vector<uint32_t> formats;

  void OnFormat(uint32_t format) override { formats.push_back(format); }
};

// ── wl_seat ──────────────────────────────────────────────────────────────────

class WlSeatInfo : public wayland::client::CWlSeat<WlSeatInfo> {
 public:
  GlobalInfo global;
  uint32_t   capabilities = 0;
  std::string name;

  void OnCapabilities(uint32_t caps) override { capabilities = caps; }
  void OnName(const char* n) override { name = n ? n : ""; }
};

// ── wl_output ────────────────────────────────────────────────────────────────

class WlOutputInfo : public wayland::client::CWlOutput<WlOutputInfo> {
 public:
  GlobalInfo global;
  int32_t x = 0;
  int32_t y = 0;
  int32_t phys_width  = 0;
  int32_t phys_height = 0;
  uint32_t subpixel    = 0;
  uint32_t transform   = 0;
  int32_t scale       = 1;
  std::string make, model;
  std::string output_name, description;
  std::vector<OutputMode> modes;

  void OnGeometry(int32_t x_,
                  int32_t y_,
                  int32_t pw,
                  int32_t ph,
                  uint32_t sub,
                  const char* mk,
                  const char* mdl,
                  uint32_t tr) override {
    x = x_;
    y = y_;
    phys_width  = pw;
    phys_height = ph;
    subpixel    = sub;
    make        = mk  ? mk  : "";
    model       = mdl ? mdl : "";
    transform   = tr;
  }
  void OnMode(uint32_t flags, int32_t w, int32_t h, int32_t refresh) override {
    modes.push_back({flags, w, h, refresh});
  }
  void OnDone() override {}
  void OnScale(int32_t factor) override { scale = factor; }
  void OnName(const char* n) override { output_name = n ? n : ""; }
  void OnDescription(const char* d) override { description = d ? d : ""; }
};

// ── zxdg_output_manager_v1 (no events — use Attach(), not _SetProxy()) ────────

#if defined(HAVE_XDG_OUTPUT)

class WlXdgOutputManager
    : public xdg_output_unstable_v1::client::CZxdgOutputManagerV1<
          WlXdgOutputManager> {
 public:
  // zxdg_output_manager_v1 has no events; provide the required ProcessEvent
  // stub so the class is concrete (CEventMap::ProcessEvent is pure virtual).
  bool ProcessEvent(uint32_t /*opcode*/, void** /*args*/) override {
    return false;
  }
};

// ── zxdg_output_v1 ───────────────────────────────────────────────────────────

class WlXdgOutputInfo
    : public xdg_output_unstable_v1::client::CZxdgOutputV1<WlXdgOutputInfo> {
 public:
  int32_t log_x = 0, log_y = 0;
  int32_t log_width = 0, log_height = 0;
  std::string name, description;

  void OnLogicalPosition(int32_t x, int32_t y) override {
    log_x = x;
    log_y = y;
  }
  void OnLogicalSize(int32_t w, int32_t h) override {
    log_width  = w;
    log_height = h;
  }
  void OnDone() override {}
  void OnName(const char* n) override { name = n ? n : ""; }
  void OnDescription(const char* d) override { description = d ? d : ""; }
};

#endif  // HAVE_XDG_OUTPUT

// ── zwp_linux_dmabuf_v1 ───────────────────────────────────────────────────────

#if defined(HAVE_LINUX_DMABUF)

class WlLinuxDmabufInfo
    : public linux_dmabuf_unstable_v1::client::CZwpLinuxDmabufV1<
          WlLinuxDmabufInfo> {
 public:
  struct Modifier {
    uint32_t format;
    uint64_t modifier;
  };
  std::vector<uint32_t> formats;
  std::vector<Modifier> modifiers;

  void OnFormat(uint32_t format) override { formats.push_back(format); }
  void OnModifier(uint32_t format,
                  uint32_t modifier_hi,
                  uint32_t modifier_lo) override {
    const uint64_t mod =
        (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo;
    modifiers.push_back({format, mod});
  }
};

#endif  // HAVE_LINUX_DMABUF

// ══════════════════════════════════════════════════════════════════════════════
// Printing helpers
// ══════════════════════════════════════════════════════════════════════════════

// Map wl_shm_format enum value → short name string (returns nullptr if unknown).
static const char* shm_format_name(uint32_t fmt) {
  // clang-format off
  switch (fmt) {
    case WL_SHM_FORMAT_ARGB8888:    return "ARGB8888";
    case WL_SHM_FORMAT_XRGB8888:    return "XRGB8888";
    case WL_SHM_FORMAT_C8:          return "C8";
    case WL_SHM_FORMAT_RGB332:      return "RGB332";
    case WL_SHM_FORMAT_BGR233:      return "BGR233";
    case WL_SHM_FORMAT_XRGB4444:    return "XRGB4444";
    case WL_SHM_FORMAT_XBGR4444:    return "XBGR4444";
    case WL_SHM_FORMAT_RGBX4444:    return "RGBX4444";
    case WL_SHM_FORMAT_BGRX4444:    return "BGRX4444";
    case WL_SHM_FORMAT_ARGB4444:    return "ARGB4444";
    case WL_SHM_FORMAT_ABGR4444:    return "ABGR4444";
    case WL_SHM_FORMAT_RGBA4444:    return "RGBA4444";
    case WL_SHM_FORMAT_BGRA4444:    return "BGRA4444";
    case WL_SHM_FORMAT_XRGB1555:    return "XRGB1555";
    case WL_SHM_FORMAT_XBGR1555:    return "XBGR1555";
    case WL_SHM_FORMAT_RGBX5551:    return "RGBX5551";
    case WL_SHM_FORMAT_BGRX5551:    return "BGRX5551";
    case WL_SHM_FORMAT_ARGB1555:    return "ARGB1555";
    case WL_SHM_FORMAT_ABGR1555:    return "ABGR1555";
    case WL_SHM_FORMAT_RGBA5551:    return "RGBA5551";
    case WL_SHM_FORMAT_BGRA5551:    return "BGRA5551";
    case WL_SHM_FORMAT_RGB565:      return "RGB565";
    case WL_SHM_FORMAT_BGR565:      return "BGR565";
    case WL_SHM_FORMAT_RGB888:      return "RGB888";
    case WL_SHM_FORMAT_BGR888:      return "BGR888";
    case WL_SHM_FORMAT_XBGR8888:    return "XBGR8888";
    case WL_SHM_FORMAT_RGBX8888:    return "RGBX8888";
    case WL_SHM_FORMAT_BGRX8888:    return "BGRX8888";
    case WL_SHM_FORMAT_ABGR8888:    return "ABGR8888";
    case WL_SHM_FORMAT_RGBA8888:    return "RGBA8888";
    case WL_SHM_FORMAT_BGRA8888:    return "BGRA8888";
    case WL_SHM_FORMAT_XRGB2101010: return "XRGB2101010";
    case WL_SHM_FORMAT_XBGR2101010: return "XBGR2101010";
    case WL_SHM_FORMAT_RGBX1010102: return "RGBX1010102";
    case WL_SHM_FORMAT_BGRX1010102: return "BGRX1010102";
    case WL_SHM_FORMAT_ARGB2101010: return "ARGB2101010";
    case WL_SHM_FORMAT_ABGR2101010: return "ABGR2101010";
    case WL_SHM_FORMAT_RGBA1010102: return "RGBA1010102";
    case WL_SHM_FORMAT_BGRA1010102: return "BGRA1010102";
    case WL_SHM_FORMAT_YUYV:        return "YUYV";
    case WL_SHM_FORMAT_YVYU:        return "YVYU";
    case WL_SHM_FORMAT_UYVY:        return "UYVY";
    case WL_SHM_FORMAT_VYUY:        return "VYUY";
    case WL_SHM_FORMAT_AYUV:        return "AYUV";
    case WL_SHM_FORMAT_NV12:        return "NV12";
    case WL_SHM_FORMAT_NV21:        return "NV21";
    case WL_SHM_FORMAT_NV16:        return "NV16";
    case WL_SHM_FORMAT_NV61:        return "NV61";
    case WL_SHM_FORMAT_YUV410:      return "YUV410";
    case WL_SHM_FORMAT_YVU410:      return "YVU410";
    case WL_SHM_FORMAT_YUV411:      return "YUV411";
    case WL_SHM_FORMAT_YVU411:      return "YVU411";
    case WL_SHM_FORMAT_YUV420:      return "YUV420";
    case WL_SHM_FORMAT_YVU420:      return "YVU420";
    case WL_SHM_FORMAT_YUV422:      return "YUV422";
    case WL_SHM_FORMAT_YVU422:      return "YVU422";
    case WL_SHM_FORMAT_YUV444:      return "YUV444";
    case WL_SHM_FORMAT_YVU444:      return "YVU444";
    default:                         return nullptr;
  }
  // clang-format on
}

static const char* subpixel_name(uint32_t sub) {
  switch (sub) {
    case WL_OUTPUT_SUBPIXEL_UNKNOWN:          return "unknown";
    case WL_OUTPUT_SUBPIXEL_NONE:             return "none";
    case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB:   return "horizontal_rgb";
    case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR:   return "horizontal_bgr";
    case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:     return "vertical_rgb";
    case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:     return "vertical_bgr";
    default:                                   return "unknown";
  }
}

static const char* transform_name(uint32_t tr) {
  switch (tr) {
    case WL_OUTPUT_TRANSFORM_NORMAL:      return "normal";
    case WL_OUTPUT_TRANSFORM_90:          return "90";
    case WL_OUTPUT_TRANSFORM_180:         return "180";
    case WL_OUTPUT_TRANSFORM_270:         return "270";
    case WL_OUTPUT_TRANSFORM_FLIPPED:     return "flipped";
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:  return "flipped_90";
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: return "flipped_180";
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: return "flipped_270";
    default:                               return "unknown";
  }
}

static void print_shm(const WlShmInfo& shm) {
  std::printf("\tpixel formats:\n");
  for (const uint32_t fmt : shm.formats) {
    const char* name = shm_format_name(fmt);
    if (name)
      std::printf("\t\t0x%08" PRIx32 " / WL_SHM_FORMAT_%s\n", fmt, name);
    else
      std::printf("\t\t0x%08" PRIx32 "\n", fmt);
  }
}

static void print_seat(const WlSeatInfo& seat) {
  if (!seat.name.empty())
    std::printf("\tname: %s\n", seat.name.c_str());
  std::printf("\tcapabilities:\n");
  if (seat.capabilities & WL_SEAT_CAPABILITY_POINTER)
    std::printf("\t\tpointer\n");
  if (seat.capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
    std::printf("\t\tkeyboard\n");
  if (seat.capabilities & WL_SEAT_CAPABILITY_TOUCH)
    std::printf("\t\ttouch\n");
}

static void print_output(const WlOutputInfo& out
#if defined(HAVE_XDG_OUTPUT)
                         ,
                         const WlXdgOutputInfo* xdg
#endif
) {
  std::printf("\tx: %" PRId32 ", y: %" PRId32 ",\n", out.x, out.y);
  std::printf("\tphysical_width: %" PRId32 " mm,"
              " physical_height: %" PRId32 " mm,\n",
              out.phys_width, out.phys_height);
  if (!out.make.empty())
    std::printf("\tmake: '%s', model: '%s',\n",
                out.make.c_str(), out.model.c_str());
  std::printf("\tsubpixel_orientation: %s, transform: %s,\n",
              subpixel_name(out.subpixel), transform_name(out.transform));
  std::printf("\tscale: %" PRId32 ",\n", out.scale);
  if (!out.output_name.empty())
    std::printf("\tname: %s,\n", out.output_name.c_str());
  if (!out.description.empty())
    std::printf("\tdescription: %s,\n", out.description.c_str());
  for (const auto& mode : out.modes) {
    std::printf("\tmode: width: %" PRId32 " px, height: %" PRId32 " px,"
                " refresh: %.3f Hz, flags:",
                mode.width, mode.height, mode.refresh / 1000.0);
    if (mode.flags & WL_OUTPUT_MODE_CURRENT)
      std::printf(" current");
    if (mode.flags & WL_OUTPUT_MODE_PREFERRED)
      std::printf(" preferred");
    std::printf(",\n");
  }
#if defined(HAVE_XDG_OUTPUT)
  if (xdg) {
    std::printf("\tlogical_x: %" PRId32 ", logical_y: %" PRId32 ",\n",
                xdg->log_x, xdg->log_y);
    std::printf("\tlogical_width: %" PRId32 ", logical_height: %" PRId32 ",\n",
                xdg->log_width, xdg->log_height);
    if (!xdg->name.empty())
      std::printf("\txdg_name: %s,\n", xdg->name.c_str());
    if (!xdg->description.empty())
      std::printf("\txdg_description: %s,\n", xdg->description.c_str());
  }
#endif
}

#if defined(HAVE_LINUX_DMABUF)
static void print_dmabuf(const WlLinuxDmabufInfo& dmabuf) {
  std::printf("\tformats (fourcc):\n");
  for (const uint32_t fmt : dmabuf.formats)
    std::printf("\t\t0x%08" PRIx32 "\n", fmt);
  if (!dmabuf.modifiers.empty()) {
    std::printf("\tformat + modifier pairs:\n");
    for (const auto& m : dmabuf.modifiers)
      std::printf("\t\t0x%08" PRIx32 " / 0x%016" PRIx64 "\n",
                  m.format, m.modifier);
  }
}
#endif

// ══════════════════════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════════════════════

int main() {
  wl_display* display = wl_display_connect(nullptr);
  if (!display) {
    std::fprintf(stderr, "wayland-info: wl_display_connect failed: %s\n",
                 std::strerror(errno));
    return EXIT_FAILURE;
  }

  wl::CRegistry registry;
  if (!registry.Create(display)) {
    std::fprintf(stderr, "wayland-info: wl_display_get_registry failed\n");
    wl_display_disconnect(display);
    return EXIT_FAILURE;
  }

  // ── Per-global bookkeeping ─────────────────────────────────────────────────
  std::vector<GlobalInfo> globals;

  // Core protocol objects
  WlShmInfo shm;
  bool shm_found = false;
  std::vector<std::unique_ptr<WlSeatInfo>>   seats;
  std::vector<std::unique_ptr<WlOutputInfo>> outputs;

  // Extended protocol objects (compiled in only when available)
#if defined(HAVE_XDG_OUTPUT)
  wl::WlPtr<WlXdgOutputManager> xdg_mgr;
  std::vector<std::unique_ptr<WlXdgOutputInfo>> xdg_outputs;
#endif
#if defined(HAVE_LINUX_DMABUF)
  std::unique_ptr<WlLinuxDmabufInfo> dmabuf;
#endif

  // ── Registry listener ──────────────────────────────────────────────────────
  registry.OnGlobal([&](wl::CRegistry& reg,
                        uint32_t name,
                        std::string_view iface,
                        uint32_t ver) {
    globals.push_back({name, std::string(iface), ver});

    using wl_shm_tr    = wayland::client::wl_shm_traits;
    using wl_seat_tr   = wayland::client::wl_seat_traits;
    using wl_output_tr = wayland::client::wl_output_traits;

    if (iface == wl_shm_tr::interface_name) {
      // Bind at version 1 (format event only; release added in v2).
      wl_proxy* raw = reg.Bind<wl_shm_tr>(name, 1u);
      if (raw) {
        shm._SetProxy(raw);
        shm_found = true;
      }

    } else if (iface == wl_seat_tr::interface_name) {
      auto seat = std::make_unique<WlSeatInfo>();
      seat->global = {name, std::string(iface), ver};
      wl_proxy* raw =
          reg.Bind<wl_seat_tr>(name, std::min(ver, wl_seat_tr::version));
      if (raw) {
        seat->_SetProxy(raw);
        seats.push_back(std::move(seat));
      }

    } else if (iface == wl_output_tr::interface_name) {
      auto out = std::make_unique<WlOutputInfo>();
      out->global = {name, std::string(iface), ver};
      wl_proxy* raw =
          reg.Bind<wl_output_tr>(name, std::min(ver, wl_output_tr::version));
      if (raw) {
        out->_SetProxy(raw);
        outputs.push_back(std::move(out));
      }

#if defined(HAVE_XDG_OUTPUT)
    } else if (iface == xdg_output_unstable_v1::client::
                             zxdg_output_manager_v1_traits::interface_name) {
      using mgr_tr = xdg_output_unstable_v1::client::zxdg_output_manager_v1_traits;
      wl_proxy* raw = reg.Bind<mgr_tr>(name, std::min(ver, mgr_tr::version));
      if (raw)
        // Use Attach() — the manager has no events, so no listener table is
        // generated and _SetProxy() would fail to compile.
        xdg_mgr.Attach(raw);
#endif

#if defined(HAVE_LINUX_DMABUF)
    } else if (iface == linux_dmabuf_unstable_v1::client::
                            zwp_linux_dmabuf_v1_traits::interface_name) {
      using dmabuf_tr = linux_dmabuf_unstable_v1::client::zwp_linux_dmabuf_v1_traits;
      // Bind at version 3 to receive both format (v1) and modifier (v3) events.
      wl_proxy* raw = reg.Bind<dmabuf_tr>(name, std::min(ver, 3u));
      if (raw) {
        dmabuf = std::make_unique<WlLinuxDmabufInfo>();
        dmabuf->_SetProxy(raw);
      }
#endif
    }
  });

  // ── First roundtrip: collect globals and wl_output / wl_seat / wl_shm
  //    events (geometry, mode, scale, capabilities, formats).  ────────────────
  wl_display_roundtrip(display);

#if defined(HAVE_XDG_OUTPUT)
  // Create one zxdg_output_v1 per wl_output if the manager was advertised.
  if (!xdg_mgr.IsNull()) {
    using mgr_traits = xdg_output_unstable_v1::client::zxdg_output_manager_v1_traits;
    using out_traits = xdg_output_unstable_v1::client::zxdg_output_v1_traits;

    for (const auto& out : outputs) {
      wl_proxy* raw =
          wl::construct<out_traits, mgr_traits::Op::GetXdgOutput>(
              *xdg_mgr.Get(), out->GetProxy());
      if (raw) {
        auto xdg = std::make_unique<WlXdgOutputInfo>();
        xdg->_SetProxy(raw);
        xdg_outputs.push_back(std::move(xdg));
      }
    }
  }
#endif

  // ── Second roundtrip: collect xdg_output logical position/size and
  //    linux_dmabuf format/modifier events.  ───────────────────────────────────
  wl_display_roundtrip(display);

  // ── Print all globals in registry order ────────────────────────────────────
  for (const auto& g : globals) {
    std::printf("interface: '%s', version: %" PRIu32 ", name: %" PRIu32 "\n",
                g.interface.c_str(), g.version, g.name);

    if (g.interface == "wl_shm" && shm_found) {
      print_shm(shm);

    } else if (g.interface == "wl_seat") {
      for (const auto& seat : seats) {
        if (seat->global.name == g.name) {
          print_seat(*seat);
          break;
        }
      }

    } else if (g.interface == "wl_output") {
      for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i]->global.name == g.name) {
#if defined(HAVE_XDG_OUTPUT)
          const WlXdgOutputInfo* xdg =
              (i < xdg_outputs.size()) ? xdg_outputs[i].get() : nullptr;
          print_output(*outputs[i], xdg);
#else
          print_output(*outputs[i]);
#endif
          break;
        }
      }

#if defined(HAVE_LINUX_DMABUF)
    } else if (g.interface == "zwp_linux_dmabuf_v1" && dmabuf) {
      print_dmabuf(*dmabuf);
#endif
    }
  }

  // ── Cleanup ────────────────────────────────────────────────────────────────
  // Destroy proxies before disconnecting so libwayland can flush any pending
  // destroy requests and avoid "leaked proxy" debug warnings.
#if defined(HAVE_XDG_OUTPUT)
  for (auto& xdg : xdg_outputs)
    if (xdg && !xdg->IsNull())
      xdg->Destroy();
  if (!xdg_mgr.IsNull())
    xdg_mgr->Destroy();
#endif
#if defined(HAVE_LINUX_DMABUF)
  if (dmabuf && !dmabuf->IsNull())
    dmabuf->Destroy();
#endif
  for (auto& seat : seats)
    if (seat && !seat->IsNull())
      seat->Destroy();
  for (auto& out : outputs)
    if (out && !out->IsNull())
      out->Destroy();
  if (shm_found)
    shm.Destroy();

  wl_display_disconnect(display);
  return EXIT_SUCCESS;
}
