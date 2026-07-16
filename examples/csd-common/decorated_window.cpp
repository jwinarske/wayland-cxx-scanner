// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// decorated_window — see decorated_window.hpp.

// clang-tidy: suppress diagnostics common to Wayland C-API boundary code.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,
//             cppcoreguidelines-pro-type-reinterpret-cast)

#include "decorated_window.hpp"

// The generated protocol headers, then the framework headers that complete
// them. Order matters: <wl/xdg_shell.hpp> defines the wl_iface() bodies the
// generated traits declare, so it must follow them.
#include "viewporter_client.hpp"
#include "wayland_client.hpp"
#include "xdg_decoration_unstable_v1_client.hpp"
#include "xdg_shell_client.hpp"

#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/scale_policy.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_decoration.hpp>
#include <wl/xdg_shell.hpp>

extern "C" {
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace wl::csd {
namespace {

using xdg_decoration_unstable_v1::client::zxdg_decoration_manager_v1_traits;
using xdg_decoration_unstable_v1::client::ZxdgToplevelDecorationV1Mode;
using xdg_shell::client::xdg_surface_traits;
using xdg_shell::client::xdg_toplevel_traits;

/// Send a request on a proxy this does not own.
///
/// The toplevel is the application's: it has its own handler on it already, so
/// it arrives here as a bare proxy. Wrapping it in a generated class to reach
/// the request methods would install a second dispatcher on a proxy that
/// already has one, and take ownership this has no business taking. The
/// generated methods are a thin wrapper over exactly this call, so going direct
/// costs only the opcode.
template <typename... Args>
void Marshal(wl_proxy* proxy, uint32_t opcode, Args... args) noexcept {
  if (proxy != nullptr)
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    wl_proxy_marshal(proxy, opcode, args...);
}

// Event-less proxies: these take a bare Attach(), since no dispatcher is
// generated for an interface with no events.
class SurfaceHandler : public wayland::client::CWlSurface<SurfaceHandler> {};
class SubsurfaceHandler
    : public wayland::client::CWlSubsurface<SubsurfaceHandler> {};
class ShmPoolHandler : public wayland::client::CWlShmPool<ShmPoolHandler> {};
class CompositorHandler
    : public wayland::client::CWlCompositor<CompositorHandler> {};
class SubcompositorHandler
    : public wayland::client::CWlSubcompositor<SubcompositorHandler> {};
// wl_shm has a format event, but the frame asks for ARGB8888 — which every
// compositor supports — so there is nothing worth listening for.
class ShmHandler : public wayland::client::CWlShm<ShmHandler> {};

// wp_viewporter / wp_viewport have no events. The viewport is what lets the
// decoration honor a fractional scale: without one the buffer is taken at face
// value and only a whole-number buffer scale is expressible, so 125% would put
// the frame on screen a quarter larger than the window it frames.
class ViewporterHandler
    : public viewporter::client::CWpViewporter<ViewporterHandler> {};
class ViewportHandler
    : public viewporter::client::CWpViewport<ViewportHandler> {};
class DecorationManagerHandler
    : public xdg_decoration_unstable_v1::client::CZxdgDecorationManagerV1<
          DecorationManagerHandler> {};

// wl_buffer has a release event, so it gets a real handler.
class BufferHandler : public wayland::client::CWlBuffer<BufferHandler> {
 public:
  bool busy = false;
  void OnRelease() override { busy = false; }
};

constexpr int kNumBuffers = 2;

// Fallbacks for a caller with no plugin to ask. Only reachable through the
// gesture code, which a plugin-less frame never runs.
constexpr int kFallbackDoubleClickMs = 400;
constexpr int kFallbackDragThreshold = 8;

/// A mapped SHM region and the buffers carved out of it.
struct Pool {
  int fd = -1;
  void* data = MAP_FAILED;
  std::size_t size = 0;
  std::array<wl::WlPtr<BufferHandler>, kNumBuffers> bufs;
  int next = 0;
  int width = 0;
  int height = 0;

  Pool() = default;
  ~Pool() { Unmap(); }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool(Pool&&) = delete;
  Pool& operator=(Pool&&) = delete;

  void Unmap() noexcept {
    if (data != MAP_FAILED) {
      munmap(data, size);
      data = MAP_FAILED;
    }
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
    size = 0;
  }

  [[nodiscard]] bool Create(int w, int h, wl_proxy* shm) noexcept {
    width = w;
    height = h;
    const std::size_t stride = static_cast<std::size_t>(w) * 4u;
    const std::size_t per = stride * static_cast<std::size_t>(h);
    const std::size_t total = per * kNumBuffers;

    fd = memfd_create("wl-csd-frame", 0);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(total)) < 0)
      return false;
    data = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
      return false;
    size = total;

    wl::WlPtr<ShmPoolHandler> pool;
    wl_shm_pool* raw = wl_shm_create_pool(reinterpret_cast<wl_shm*>(shm), fd,
                                          static_cast<int>(total));
    if (raw == nullptr)
      return false;
    pool.Attach(reinterpret_cast<wl_proxy*>(raw));

    for (int i = 0; i < kNumBuffers; ++i) {
      wl_proxy* b =
          wl::construct<wayland::client::wl_buffer_traits,
                        wayland::client::wl_shm_pool_traits::Op::CreateBuffer>(
              *pool.Get(),
              static_cast<int32_t>(static_cast<std::size_t>(i) * per), w, h,
              static_cast<int32_t>(stride), WL_SHM_FORMAT_ARGB8888);
      if (b == nullptr)
        return false;
      bufs.at(static_cast<std::size_t>(i)).Get()->_SetProxy(b);
    }
    return true;
  }

  [[nodiscard]] void* Pixels(int i) const noexcept {
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    return static_cast<uint8_t*>(data) + static_cast<std::size_t>(i) * stride *
                                             static_cast<std::size_t>(height);
  }

  [[nodiscard]] int NextFree() noexcept {
    for (int a = 0; a < kNumBuffers; ++a) {
      const int i = (next + a) % kNumBuffers;
      if (!bufs.at(static_cast<std::size_t>(i)).Get()->busy) {
        next = (i + 1) % kNumBuffers;
        return i;
      }
    }
    return -1;
  }

  [[nodiscard]] bool AllReleased() const noexcept {
    for (const auto& b : bufs) {
      if (!b.IsNull() && b.Get()->busy)
        return false;
    }
    return true;
  }
};

}  // namespace

// ══════════════════════════════════════════════════════════════════════════════
// Impl
// ══════════════════════════════════════════════════════════════════════════════

struct DecoratedWindow::Impl {
  std::unique_ptr<CsdPlugin> plugin;
  wl_proxy* content = nullptr;
  wl_proxy* seat = nullptr;
  wl_proxy* toplevel = nullptr;
  wl_proxy* xdg_surface = nullptr;

  // Bound from a registry of this frame's own, so a caller never has to hand
  // over globals it does not otherwise use.
  wl::CRegistry registry;
  wl::WlPtr<CompositorHandler> compositor;
  wl::WlPtr<SubcompositorHandler> subcompositor;
  wl::WlPtr<ShmHandler> shm;
  wl::WlPtr<DecorationManagerHandler> decoration_mgr;
  // Optional: a compositor without it leaves the decoration on the whole-number
  // buffer scale below, which is right at 100% and 200% and wrong in between.
  wl::WlPtr<ViewporterHandler> viewporter;

  wl::WlPtr<SurfaceHandler> surface;
  wl::WlPtr<SubsurfaceHandler> subsurface;
  wl::WlPtr<ViewportHandler> viewport;
  wl::WlPtr<wl::XdgDecorationHandler<Impl>> decoration;

  std::unique_ptr<Pool> pool = std::make_unique<Pool>();
  // Kept alive until the compositor hands its buffers back: destroying a
  // wl_buffer that is still attached leaves the surface contents undefined.
  std::unique_ptr<Pool> retired;

  int scale_120 = ScalePolicy::kUnityScale120;
  int placed_x = INT32_MIN;
  int placed_y = INT32_MIN;
  // Last viewport destination and buffer scale submitted. Both are
  // double-buffered state, so they are only re-sent when they change.
  int viewport_w = -1;
  int viewport_h = -1;
  int buffer_scale = 0;

  // The negotiated answer. With no decoration manager the compositor has no way
  // to say, and will not decorate — so a plugin is the only chance of a frame
  // and takes the job by default.
  bool draws_client_side = false;

  // Last committed content size, so a pointer event can be hit-tested without
  // the caller passing a size it should not have to know this needs.
  int content_w = 0;
  int content_h = 0;

  // The size to go back to when the compositor stops imposing one. Tracked here
  // rather than by the caller because only this knows the difference between a
  // configure the compositor chose and one it left to us — and because the
  // caller getting it wrong is invisible until an un-maximize.
  int restore_w = 0;
  int restore_h = 0;

  // Geometry last declared, so an unchanged frame does not re-declare it.
  int geom_w = -1;
  int geom_h = -1;

  // ── Toplevel state, as the compositor reports it ──────────────────────────
  bool activated = true;
  bool maximized = false;
  bool fullscreen = false;

  // ── Pointer ───────────────────────────────────────────────────────────────
  bool over_frame = false;
  int pointer_x = -1;
  int pointer_y = -1;
  bool pressed = false;
  HitZone pressed_zone = HitZone::None;
  uint32_t enter_serial = 0;

  bool title_press_pending = false;
  uint32_t title_press_serial = 0;
  uint32_t title_press_time = 0;  // 0 ⇒ no press to pair a double-click with
  int title_press_x = 0;
  int title_press_y = 0;

  bool close_requested = false;

  // Whether the decoration surface has been taken off the screen. Not drawing
  // is not the same as removing what was drawn: the subsurface keeps its last
  // buffer until told otherwise, so going fullscreen would leave a title bar
  // stranded over the content.
  bool frame_hidden = false;

  void HideFrame() noexcept {
    if (surface.IsNull() || frame_hidden)
      return;
    // A surface with no buffer is not mapped, which is how a subsurface is
    // taken off the screen -- there is no other way to unmap one.
    surface.Get()->Attach(nullptr, 0, 0);
    surface.Get()->Commit();
    frame_hidden = true;
  }

  [[nodiscard]] HitZone HitTest(int x, int y) const noexcept {
    if (!plugin)
      return HitZone::None;
    const Margins m = plugin->DecorationMargins();
    return plugin->HitTest(x, y, content_w + m.left + m.right,
                           content_h + m.top + m.bottom, content_w, content_h);
  }

  void PushInputState() const {
    if (!plugin)
      return;
    plugin->SetInputState(InputState{over_frame ? pointer_x : -1,
                                     over_frame ? pointer_y : -1, pressed,
                                     activated, maximized});
  }

  [[nodiscard]] bool IsDoubleClick(uint32_t time) const noexcept {
    if (title_press_time == 0)
      return false;
    const int interval =
        plugin ? plugin->DoubleClickTimeMs() : kFallbackDoubleClickMs;
    const int threshold =
        plugin ? plugin->DragThreshold() : kFallbackDragThreshold;
    return (time - title_press_time) <= static_cast<uint32_t>(interval) &&
           std::abs(pointer_x - title_press_x) <= threshold &&
           std::abs(pointer_y - title_press_y) <= threshold;
  }

  // Request only. `maximized` follows the configure the compositor sends back,
  // so a refused request does not leave the wrong icon drawn.
  void ToggleMaximized() const noexcept {
    Marshal(toplevel, maximized ? xdg_toplevel_traits::Op::UnsetMaximized
                                : xdg_toplevel_traits::Op::SetMaximized);
  }

  /// The compositor's answer to the mode request. Without a plugin the answer
  /// is always server-side whatever it says: nothing is compiled in that could
  /// draw a frame.
  void OnDecorationConfigure(uint32_t mode) {
    const bool was = draws_client_side;
    draws_client_side =
        plugin != nullptr &&
        mode == static_cast<uint32_t>(ZxdgToplevelDecorationV1Mode::ClientSide);
    if (was != draws_client_side) {
      std::fprintf(stderr, "csd: decoration mode → %s\n",
                   draws_client_side ? "client-side" : "server-side");
    }
  }
};

DecoratedWindow::DecoratedWindow() : impl_(std::make_unique<Impl>()) {}
DecoratedWindow::~DecoratedWindow() = default;

// ══════════════════════════════════════════════════════════════════════════════
// Setup and negotiation
// ══════════════════════════════════════════════════════════════════════════════

bool DecoratedWindow::Init(const Config& config,
                           std::unique_ptr<CsdPlugin> plugin) {
  impl_->plugin = std::move(plugin);
  impl_->content = config.content_surface;
  impl_->seat = config.seat;
  impl_->toplevel = config.xdg_toplevel;
  impl_->xdg_surface = config.xdg_surface;

  // The size to restore to until a compositor imposes one. Seeded from the
  // caller's own default: the first configure is often "you pick", and this is
  // the only thing that knows what the caller would have picked.
  impl_->restore_w = std::max(1, config.content_width);
  impl_->restore_h = std::max(1, config.content_height);

  if (config.display == nullptr)
    return false;

  // A registry of this frame's own. The protocol allows any number of them, and
  // one here means a caller never binds wl_shm or wl_subcompositor purely to
  // hand them over — nor silently loses the decoration negotiation by
  // forgetting the manager.
  uint32_t compositor_name = 0, compositor_ver = 0;
  uint32_t subcompositor_name = 0, subcompositor_ver = 0;
  uint32_t shm_name = 0, shm_ver = 0;
  uint32_t deco_name = 0, deco_ver = 0;
  uint32_t viewporter_name = 0, viewporter_ver = 0;

  if (!impl_->registry.Create(config.display))
    return false;
  impl_->registry.OnGlobal([&](wl::CRegistry& /*reg*/, uint32_t name,
                               std::string_view iface, uint32_t ver) {
    if (iface == wayland::client::wl_compositor_traits::interface_name) {
      compositor_name = name;
      compositor_ver = ver;
    } else if (iface ==
               wayland::client::wl_subcompositor_traits::interface_name) {
      subcompositor_name = name;
      subcompositor_ver = ver;
    } else if (iface == wayland::client::wl_shm_traits::interface_name) {
      shm_name = name;
      shm_ver = ver;
    } else if (iface == zxdg_decoration_manager_v1_traits::interface_name) {
      deco_name = name;
      deco_ver = ver;
    } else if (iface ==
               viewporter::client::wp_viewporter_traits::interface_name) {
      viewporter_name = name;
      viewporter_ver = ver;
    }
  });
  if (!wl::RoundtripWithTimeout(config.display))
    return false;

  auto bind = [&](auto& holder, auto traits_tag, uint32_t name,
                  uint32_t ver) -> bool {
    using Traits = decltype(traits_tag);
    if (name == 0)
      return false;
    wl_proxy* raw =
        impl_->registry.Bind<Traits>(name, std::min(ver, Traits::version));
    if (raw == nullptr)
      return false;
    holder.Attach(raw);
    return true;
  };
  bind(impl_->compositor, wayland::client::wl_compositor_traits{},
       compositor_name, compositor_ver);
  bind(impl_->subcompositor, wayland::client::wl_subcompositor_traits{},
       subcompositor_name, subcompositor_ver);
  bind(impl_->shm, wayland::client::wl_shm_traits{}, shm_name, shm_ver);
  // Optional: a compositor offering no decoration manager cannot be asked to
  // decorate, and will not.
  bind(impl_->decoration_mgr, zxdg_decoration_manager_v1_traits{}, deco_name,
       deco_ver);
  // Optional: without it the decoration falls back to a whole-number buffer
  // scale, which cannot express 125%.
  bind(impl_->viewporter, viewporter::client::wp_viewporter_traits{},
       viewporter_name, viewporter_ver);

  // Ask for client-side only when we both can and want to. With no plugin there
  // is nothing to draw a frame with, and when the build prefers the compositor
  // it gets first refusal; either way the compositor has the last word and
  // answers with a configure.
  const bool want_csd = impl_->plugin != nullptr && !config.prefer_server_side;

  if (!impl_->decoration_mgr.IsNull() && config.xdg_toplevel != nullptr) {
    // The manager is borrowed too, so the decoration is constructed by hand
    // rather than through wl::construct, which wants an owning handler. The
    // decoration itself *is* owned from here: it carries the configure that
    // settles who draws.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    wl_proxy* raw = wl_proxy_marshal_constructor(
        impl_->decoration_mgr.Get()->GetProxy(),
        zxdg_decoration_manager_v1_traits::Op::GetToplevelDecoration,
        &wl::xdg_decoration::decoration_iface, nullptr, config.xdg_toplevel);
    if (raw != nullptr && wl::SetupHandler(impl_->decoration, raw)) {
      impl_->decoration.Get()->app_ = impl_.get();
      impl_->decoration.Get()->SetMode(static_cast<uint32_t>(
          want_csd ? ZxdgToplevelDecorationV1Mode::ClientSide
                   : ZxdgToplevelDecorationV1Mode::ServerSide));
    }
  } else {
    // No manager: the compositor cannot be asked and will not decorate. The
    // plugin is the only chance of a frame, so it takes the job — and with no
    // plugin the window is simply undecorated, which is a supported state.
    impl_->draws_client_side = impl_->plugin != nullptr;
  }

  // No plugin: nothing to build, and every other call becomes a no-op. A caller
  // built for server-side decorations needs no special case.
  if (!impl_->plugin)
    return true;

  // No compositor, subcompositor or shm: nowhere to put a decoration. The
  // window is then undecorated unless the compositor took the job above, which
  // is a worse outcome than a frame but not a broken one.
  if (impl_->compositor.IsNull() || impl_->subcompositor.IsNull() ||
      impl_->shm.IsNull() || config.content_surface == nullptr)
    return false;

  // Create the decoration surface and make it a child of the content surface.
  wl_surface* raw_surface = wl_compositor_create_surface(
      reinterpret_cast<wl_compositor*>(impl_->compositor.Get()->GetProxy()));
  if (raw_surface == nullptr)
    return false;
  // _SetProxy, not Attach: wl_surface has events, so it takes the generated
  // dispatcher — and CWlSurface::Attach is the protocol's attach request, which
  // hides the base's.
  if (!wl::SetupHandler(impl_->surface,
                        reinterpret_cast<wl_proxy*>(raw_surface)))
    return false;

  wl_subsurface* raw_sub = wl_subcompositor_get_subsurface(
      reinterpret_cast<wl_subcompositor*>(
          impl_->subcompositor.Get()->GetProxy()),
      raw_surface, reinterpret_cast<wl_surface*>(config.content_surface));
  if (raw_sub == nullptr)
    return false;
  // wl_subsurface has no events, so a bare Attach is right here.
  impl_->subsurface.Attach(reinterpret_cast<wl_proxy*>(raw_sub));

  // A viewport of the decoration's own, so Commit() can present a buffer
  // rendered at any scale back at the surface's logical size. The content
  // surface's viewport is the application's and covers only the application's
  // surface; a subsurface is a surface like any other and needs its own.
  if (!impl_->viewporter.IsNull()) {
    if (wl_proxy* raw = wl::construct<
            viewporter::client::wp_viewport_traits,
            viewporter::client::wp_viewporter_traits::Op::GetViewport>(
            *impl_->viewporter.Get(), impl_->surface.Get()->GetProxy())) {
      impl_->viewport.Attach(raw);
    }
  }

  // Behind the content: the plugin leaves the content rectangle untouched, and
  // the application's surface covers it.
  impl_->subsurface.Get()->PlaceBelow(config.content_surface);
  // Synchronized (the default) is what is wanted: the frame then appears with
  // the content it belongs to rather than a frame ahead of it during a resize.
  return true;
}

bool DecoratedWindow::DrawsClientSide() const noexcept {
  // Fullscreen outranks the negotiation: the compositor gave the client the
  // whole output, so there is no window to put a frame around. Every margin
  // then reports zero and the geometry becomes the content, which is what a
  // fullscreen surface wants.
  return impl_->draws_client_side && impl_->plugin != nullptr &&
         !impl_->fullscreen;
}

// ══════════════════════════════════════════════════════════════════════════════
// Geometry
// ══════════════════════════════════════════════════════════════════════════════
//
// Every one of these answers zero-margin when the frame is not drawing, so a
// caller needs no test of its own: on server-side decorations the content is
// the window, which is exactly what these then say.

Margins DecoratedWindow::DecorationMargins() const {
  return DrawsClientSide() ? impl_->plugin->DecorationMargins() : Margins{};
}

Margins DecoratedWindow::VisibleMargins() const {
  return DrawsClientSide() ? impl_->plugin->VisibleMargins() : Margins{};
}

void DecoratedWindow::ContentSizeForConfigure(int width,
                                              int height,
                                              int* content_w,
                                              int* content_h) {
  // A compositor's dimension reaches an allocation, so it is clamped before it
  // is believed. Same reasoning as the scale clamp: the value is not ours.
  static constexpr int kMaxDim = 16384;

  int cw = 0;
  int ch = 0;
  if (width > 0 && height > 0) {
    // Only the decoration that is part of the window: a shadow lies outside the
    // window geometry the compositor just sized, so subtracting it here would
    // shrink the content by the shadow on every configure. This is the exact
    // inverse of the rectangle Commit() declares.
    const Margins v = VisibleMargins();
    cw = std::min(width, kMaxDim) - v.left - v.right;
    ch = std::min(height, kMaxDim) - v.top - v.bottom;
  } else {
    // A zero axis means the compositor has no opinion and the size is ours to
    // pick. It is how a compositor says "go back to whatever you were", so this
    // is the path an un-maximize takes -- and ignoring it leaves the window
    // holding its maximized buffer, still looking maximized while the
    // compositor believes it was restored. The compositor then has nothing to
    // change on the next maximize, so it stays silent and the window is stuck
    // for good.
    //
    // Only the zero axes are ours to choose; the other still binds.
    const Margins v = VisibleMargins();
    cw = width > 0 ? std::min(width, kMaxDim) - v.left - v.right
                   : impl_->restore_w;
    ch = height > 0 ? std::min(height, kMaxDim) - v.top - v.bottom
                    : impl_->restore_h;
  }

  cw = std::clamp(cw, 1, kMaxDim);
  ch = std::clamp(ch, 1, kMaxDim);

  // Remember the size to come back to, but only while the compositor is not
  // imposing one: a maximized or fullscreen size is the compositor's, not a
  // size we would ever choose to return to.
  if (!impl_->maximized && !impl_->fullscreen) {
    impl_->restore_w = cw;
    impl_->restore_h = ch;
  }

  *content_w = cw;
  *content_h = ch;
}

// ══════════════════════════════════════════════════════════════════════════════
// State
// ══════════════════════════════════════════════════════════════════════════════

void DecoratedWindow::SetTitle(std::string_view title) {
  if (impl_->plugin)
    impl_->plugin->SetTitle(title);
}

void DecoratedWindow::SetScale(int scale_120) {
  impl_->scale_120 = scale_120;
  if (impl_->plugin)
    impl_->plugin->SetScale(scale_120);
}

void DecoratedWindow::SetToplevelStates(bool activated,
                                        bool maximized,
                                        bool fullscreen) noexcept {
  impl_->activated = activated;
  impl_->maximized = maximized;
  impl_->fullscreen = fullscreen;
  impl_->PushInputState();
}

void DecoratedWindow::Dispatch() {
  if (impl_->plugin)
    impl_->plugin->Dispatch();
}

// ══════════════════════════════════════════════════════════════════════════════
// Frame
// ══════════════════════════════════════════════════════════════════════════════

void DecoratedWindow::Commit(int content_w, int content_h) {
  impl_->content_w = content_w;
  impl_->content_h = content_h;

  // Declare the window's visible bounds — the content inset by the decoration
  // that is part of the window, the shadow being outside it. This is the exact
  // rectangle ContentSizeForConfigure() reads back out of the next configure;
  // if the two ever disagree the window resizes itself by the difference on
  // every round trip, which is why neither is the caller's to compute.
  //
  // With no decoration the geometry is the content, which is what an
  // undecorated window wants anyway — so this is right on the server-side path
  // too.
  if (impl_->xdg_surface != nullptr &&
      (content_w != impl_->geom_w || content_h != impl_->geom_h)) {
    const Margins v = VisibleMargins();
    Marshal(impl_->xdg_surface, xdg_surface_traits::Op::SetWindowGeometry,
            -v.left, -v.top, v.left + content_w + v.right,
            v.top + content_h + v.bottom);
    impl_->geom_w = content_w;
    impl_->geom_h = content_h;
  }

  if (!DrawsClientSide()) {
    // Either the compositor took the job, there is no plugin, or the window is
    // fullscreen. In every case the decoration comes off the screen rather than
    // simply stopping being redrawn.
    impl_->HideFrame();
    return;
  }
  if (impl_->surface.IsNull())
    return;
  impl_->frame_hidden = false;

  const Margins m = impl_->plugin->DecorationMargins();
  const int sw = content_w + m.left + m.right;
  const int sh = content_h + m.top + m.bottom;
  const ScalePolicy::BufferSize buf =
      ScalePolicy::ToBuffer(sw, sh, impl_->scale_120);
  if (buf.width <= 0 || buf.height <= 0)
    return;

  // The compositor may still be displaying buffers from the current pool, so a
  // resize retires it rather than freeing it.
  if (impl_->pool->width != buf.width || impl_->pool->height != buf.height) {
    if (!impl_->retired || impl_->retired->AllReleased())
      impl_->retired = std::move(impl_->pool);
    impl_->pool = std::make_unique<Pool>();
    if (!impl_->pool->Create(buf.width, buf.height,
                             impl_->shm.Get()->GetProxy())) {
      std::fprintf(stderr, "csd: decoration pool %dx%d failed\n", buf.width,
                   buf.height);
      return;
    }
  }
  if (impl_->retired && impl_->retired->AllReleased())
    impl_->retired.reset();

  const int idx = impl_->pool->NextFree();
  if (idx < 0) {
    // Every buffer still held. Nothing to draw into, so leave the frame as it
    // is — it is a child of the content surface and will be committed with it.
    return;
  }

  auto* pixels = static_cast<uint32_t*>(impl_->pool->Pixels(idx));
  impl_->plugin->RenderDecoration(pixels, buf.width, sw, sh, content_w,
                                  content_h);

  // The decoration sits at the negative margin: its origin is above and left of
  // the content surface's.
  if (m.left != impl_->placed_x || m.top != impl_->placed_y) {
    impl_->subsurface.Get()->SetPosition(-m.left, -m.top);
    impl_->placed_x = m.left;
    impl_->placed_y = m.top;
  }

  // Nothing here is opaque: the shadow is translucent, the corners are rounded,
  // and the content rectangle is left clear for the application's surface to
  // show through from in front. Saying so keeps the compositor from compositing
  // the frame as though it were solid.
  impl_->surface.Get()->SetOpaqueRegion(nullptr);

  // Present the buffer back at the logical size the frame was laid out in. The
  // buffer is `buf` physical pixels; the surface must measure `sw` by `sh`
  // however many pixels went into it, or the decoration lands on screen at a
  // different size from the window it frames.
  if (!impl_->viewport.IsNull()) {
    if (sw != impl_->viewport_w || sh != impl_->viewport_h) {
      impl_->viewport.Get()->SetDestination(sw, sh);
      impl_->viewport_w = sw;
      impl_->viewport_h = sh;
    }
  } else if (impl_->scale_120 != ScalePolicy::kUnityScale120) {
    // No viewporter. The buffer is then taken at face value and a whole-number
    // buffer scale is the only lever there is, so this is exact at 200% and
    // wrong by the remainder anywhere between — the frame is oversized by
    // whatever the division threw away. Better than ignoring the scale
    // outright, and every compositor that advertises a fractional scale also
    // offers the viewporter that answers it.
    const int n = impl_->scale_120 / ScalePolicy::kUnityScale120;
    if (n >= 1 && n != impl_->buffer_scale) {
      impl_->surface.Get()->SetBufferScale(n);
      impl_->buffer_scale = n;
    }
  }
  impl_->surface.Get()->Attach(
      impl_->pool->bufs.at(static_cast<std::size_t>(idx)).Get()->GetProxy(), 0,
      0);
  impl_->surface.Get()->Damage(0, 0, sw, sh);
  impl_->surface.Get()->Commit();
  impl_->pool->bufs.at(static_cast<std::size_t>(idx)).Get()->busy = true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Input
// ══════════════════════════════════════════════════════════════════════════════

bool DecoratedWindow::OwnsSurface(const wl_proxy* surface) const noexcept {
  return !impl_->surface.IsNull() && surface != nullptr &&
         surface == impl_->surface.Get()->GetProxy();
}

void DecoratedWindow::OnPointerEnter(const PointerEventLite& ev) noexcept {
  if (!OwnsSurface(ev.surface)) {
    // Entered the application's own surface: the frame is no longer under the
    // pointer, so any hover styling it is drawing has to come off.
    if (impl_->over_frame) {
      impl_->over_frame = false;
      impl_->PushInputState();
    }
    return;
  }
  impl_->over_frame = true;
  impl_->enter_serial = ev.serial;  // set_cursor must carry an enter serial
  impl_->pointer_x = static_cast<int>(ev.x);
  impl_->pointer_y = static_cast<int>(ev.y);
  impl_->PushInputState();
}

void DecoratedWindow::OnPointerLeave() noexcept {
  if (!impl_->over_frame)
    return;
  impl_->over_frame = false;
  impl_->pointer_x = -1;
  impl_->pointer_y = -1;
  impl_->pressed = false;
  impl_->pressed_zone =
      HitZone::None;  // A press the pointer leaves is canceled.
  impl_->title_press_pending = false;
  impl_->title_press_time = 0;
  impl_->PushInputState();
}

void DecoratedWindow::OnPointerMotion(const PointerEventLite& ev) noexcept {
  if (!impl_->over_frame)
    return;
  impl_->pointer_x = static_cast<int>(ev.x);
  impl_->pointer_y = static_cast<int>(ev.y);
  impl_->PushInputState();

  // A held title-bar press becomes a move once the pointer travels far enough
  // to mean it. Below the threshold it stays a click, so a double-click still
  // has a chance to happen.
  if (!impl_->title_press_pending)
    return;
  const int threshold =
      impl_->plugin ? impl_->plugin->DragThreshold() : kFallbackDragThreshold;
  if (std::abs(impl_->pointer_x - impl_->title_press_x) <= threshold &&
      std::abs(impl_->pointer_y - impl_->title_press_y) <= threshold)
    return;

  impl_->title_press_pending = false;
  impl_->title_press_time = 0;  // Became a drag: not half of a double-click.
  impl_->pressed = false;
  impl_->pressed_zone = HitZone::None;
  impl_->PushInputState();
  if (impl_->seat != nullptr) {
    Marshal(impl_->toplevel, xdg_toplevel_traits::Op::Move, impl_->seat,
            impl_->title_press_serial);
  }
}

// A window button fires on release over the button it was pressed on — the
// convention every toolkit follows, and the only one that lets a press be
// visible as a pressed state or be taken back by releasing elsewhere. Acting on
// press instead makes the pressed styling unreachable, since the window is
// already gone by the time it would be drawn.
//
// Move and resize are the exception: they are drags, so they start on press and
// the compositor takes a grab from there.
void DecoratedWindow::OnPointerButton(
    const PointerButtonEventLite& ev) noexcept {
  if (!impl_->over_frame || ev.button != BTN_LEFT)
    return;

  const HitZone zone = impl_->HitTest(impl_->pointer_x, impl_->pointer_y);

  if (ev.state == WL_POINTER_BUTTON_STATE_PRESSED) {
    impl_->pressed = true;
    impl_->pressed_zone = zone;
    impl_->PushInputState();

    const uint32_t edge = HitZoneToResizeEdge(zone);

    if (zone == HitZone::TitleBar && impl_->seat != nullptr) {
      // Do not start the move yet. xdg_toplevel.move() hands the pointer to the
      // compositor's grab, and every later event — including the second click
      // of a double-click — goes there instead of here. So the press is held:
      // it becomes a move once the pointer travels past the drag threshold (see
      // OnPointerMotion), and a maximize toggle if a second press arrives
      // first.
      if (impl_->IsDoubleClick(ev.time)) {
        impl_->ToggleMaximized();
        impl_->title_press_time = 0;  // Consumed: a third click starts over.
        impl_->pressed = false;
        impl_->pressed_zone = HitZone::None;
        impl_->PushInputState();
        return;
      }
      impl_->title_press_pending = true;
      impl_->title_press_serial = ev.serial;
      impl_->title_press_time = ev.time;
      impl_->title_press_x = impl_->pointer_x;
      impl_->title_press_y = impl_->pointer_y;
      return;
    }

    if (edge != 0 && impl_->seat != nullptr) {
      Marshal(impl_->toplevel, xdg_toplevel_traits::Op::Resize, impl_->seat,
              ev.serial, edge);
    } else {
      return;  // A button: hold the press and wait for the release.
    }

    // The compositor owns the pointer for the duration of the grab and the
    // matching release never arrives, so the press is finished with here.
    impl_->pressed = false;
    impl_->pressed_zone = HitZone::None;
    impl_->PushInputState();
    return;
  }

  // ── Release ───────────────────────────────────────────────────────────────
  const HitZone pressed = impl_->pressed_zone;
  impl_->pressed = false;
  impl_->pressed_zone = HitZone::None;
  // A title-bar press that never traveled is just a click. title_press_time
  // survives, so a second press soon after can still pair with it.
  impl_->title_press_pending = false;
  impl_->PushInputState();

  // Released somewhere other than where the press landed: canceled.
  if (pressed != zone)
    return;

  switch (zone) {
    case HitZone::CloseButton:
      // The only gesture that comes back to the caller: nothing here can decide
      // to exit an application.
      impl_->close_requested = true;
      break;

    case HitZone::MaximizeButton:
      impl_->ToggleMaximized();
      break;

    case HitZone::MinimizeButton:
      Marshal(impl_->toplevel, xdg_toplevel_traits::Op::SetMinimized);
      break;

    default:
      break;
  }
}

bool DecoratedWindow::CloseRequested() const noexcept {
  return impl_->close_requested;
}

const char* DecoratedWindow::CursorThemeName() const {
  return impl_->plugin ? impl_->plugin->CursorThemeName() : nullptr;
}

int DecoratedWindow::CursorSize() const {
  return impl_->plugin ? impl_->plugin->CursorSize() : 0;
}

const char* DecoratedWindow::CursorName() const noexcept {
  if (!impl_->over_frame || !DrawsClientSide())
    return nullptr;
  return HitZoneToCursorName(
      impl_->HitTest(impl_->pointer_x, impl_->pointer_y));
}

uint32_t DecoratedWindow::EnterSerial() const noexcept {
  return impl_->enter_serial;
}

}  // namespace wl::csd

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-pro-type-reinterpret-cast)
