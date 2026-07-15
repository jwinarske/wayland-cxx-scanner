// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// csd_gtk_backend_gtk3 — GTK 3 backend for the themed CSD plugin.
//
// The only translation unit that includes gtk.h.
//
// Nothing here decides what a header bar looks like.  A real GtkHeaderBar is
// built inside an offscreen GtkWindow, GTK lays it out and styles it from the
// active theme, and this file asks GTK to draw and measure it.  That is what
// makes the decoration match: the height, the font, which buttons exist and in
// what order, the icons, the corner radius and the hover styling are all GTK's
// answers, not ours.
//
// In particular the button set is never parsed out of a setting: GTK builds the
// header bar from gtk-decoration-layout, and the buttons are then found by
// walking the widget tree it produced.

// clang-tidy: suppress diagnostics for C-API boundary code.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,
//             cppcoreguidelines-pro-bounds-pointer-arithmetic,
//             cppcoreguidelines-avoid-non-const-global-variables)

#include "csd_gtk_backend.hpp"

#include <cstring>
#include <string>

#include <gtk/gtk.h>

namespace wl::csd::detail {
namespace {

// ══════════════════════════════════════════════════════════════════════════════
// Widget lookup
// ══════════════════════════════════════════════════════════════════════════════

/// A header element and the widget GTK built for it (null when the theme's
/// decoration layout omits it — a close-only layout has no maximize button).
///
/// `type` is what the element must actually be, and it is load-bearing rather
/// than a sanity check.  The search matches a substring of the style-context
/// dump, and those names are not disjoint: ".maximize" is a prefix of
/// ".maximized", the class added to the header while the window is maximized.
/// Without the type check, asking for the maximize button of a maximized
/// window returns the header bar itself — whose clip covers the whole title
/// bar, so every point in it would hit-test as the maximize button.
struct HeaderElement {
  const char* name = nullptr;
  GType type = G_TYPE_INVALID;
  GtkWidget* widget = nullptr;
};

/// Style-context selectors identifying each element within the header bar.
/// These are what GTK's own CSS calls them, which is why matching on them
/// works across themes.
constexpr const char* kSelTitle = "label.title:";
constexpr const char* kSelMinimize = ".minimize";
constexpr const char* kSelMaximize = ".maximize";
constexpr const char* kSelClose = ".close";

void FindByName(GtkWidget* widget, void* data) {
  auto* elem = static_cast<HeaderElement*>(data);
  if (elem->widget != nullptr)
    return;  // Already found; do not let a later sibling overwrite it.

  if (GTK_IS_WIDGET(widget) && G_TYPE_CHECK_INSTANCE_TYPE(widget, elem->type)) {
    gchar* desc =
        gtk_style_context_to_string(gtk_widget_get_style_context(widget),
                                    GTK_STYLE_CONTEXT_PRINT_SHOW_STYLE);
    const bool hit =
        desc != nullptr && std::strstr(desc, elem->name) != nullptr;
    g_free(desc);
    if (hit) {
      elem->widget = widget;
      return;
    }
  }

  if (GTK_IS_CONTAINER(widget))
    gtk_container_forall(GTK_CONTAINER(widget), &FindByName, data);
}

/// Find a header element of type @p type whose style context names @p selector.
[[nodiscard]] HeaderElement FindElement(GtkWidget* header,
                                        const char* selector,
                                        GType type) {
  HeaderElement elem{selector, type, nullptr};
  FindByName(header, &elem);
  return elem;
}

[[nodiscard]] bool InRect(const GtkAllocation& r, int x, int y) noexcept {
  return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

// ══════════════════════════════════════════════════════════════════════════════
// Gtk3Backend
// ══════════════════════════════════════════════════════════════════════════════

class Gtk3Backend final : public GtkThemeBackend {
 public:
  ~Gtk3Backend() override {
    if (GTK_IS_WIDGET(window_))
      gtk_widget_destroy(window_);  // Destroys the header with it.
  }

  [[nodiscard]] bool Init() override {
    // Keep GTK off X11 even when DISPLAY is set, and stop it calling
    // setlocale() behind the application's back.
    gdk_set_allowed_backends("wayland");
    gtk_disable_setlocale();

    // _check, not gtk_init: a missing display or theme must degrade to another
    // plugin, not abort the process.
    if (!gtk_init_check(nullptr, nullptr))
      return false;

    window_ = gtk_offscreen_window_new();
    header_ = gtk_header_bar_new();
    if (window_ == nullptr || header_ == nullptr)
      return false;

    g_object_set(header_, "title", title_.c_str(), "has-subtitle", FALSE,
                 "show-close-button", TRUE, nullptr);

    // The classes that make the theme style this as a window titlebar rather
    // than as an in-window header bar.
    GtkStyleContext* ctx = gtk_widget_get_style_context(header_);
    gtk_style_context_add_class(ctx, GTK_STYLE_CLASS_TITLEBAR);
    gtk_style_context_add_class(ctx, "default-decoration");

    gtk_window_set_titlebar(GTK_WINDOW(window_), header_);
    gtk_widget_show_all(window_);
    return true;
  }

  void SetTitle(std::string_view title) override {
    title_ = title;
    if (GTK_IS_WIDGET(header_))
      gtk_header_bar_set_title(GTK_HEADER_BAR(header_), title_.c_str());
  }

  void SetInputState(const InputState& state) override { state_ = state; }

  [[nodiscard]] int HeaderHeight() override {
    if (!GTK_IS_WIDGET(header_))
      return 0;
    int height = 0;
    gtk_widget_get_preferred_height(header_, nullptr, &height);
    return height;
  }

  void DrawHeader(cairo_surface_t* surface, int width) override {
    if (!GTK_IS_WIDGET(header_))
      return;

    ApplyWindowState();
    Layout(width);

    cairo_t* cr = cairo_create(surface);
    DrawBackground(cr);
    DrawTitle(surface);
    DrawButtons(cr, surface);
    cairo_destroy(cr);
  }

  [[nodiscard]] HitZone HitTestHeader(int x, int y, int width) override {
    if (!GTK_IS_WIDGET(header_))
      return HitZone::TitleBar;

    // The buttons have no position until the header has been laid out, so a
    // hit test arriving before the first draw would otherwise find nothing.
    LayoutIfNeeded(width);

    // Buttons before the header itself: they sit inside it.
    static constexpr struct {
      const char* sel;
      HitZone zone;
    } kButtons[] = {
        {kSelMinimize, HitZone::MinimizeButton},
        {kSelMaximize, HitZone::MaximizeButton},
        {kSelClose, HitZone::CloseButton},
    };

    for (const auto& b : kButtons) {
      GtkWidget* w = FindElement(header_, b.sel, GTK_TYPE_BUTTON).widget;
      if (w == nullptr)
        continue;
      GtkAllocation alloc{};
      gtk_widget_get_clip(w, &alloc);
      if (InRect(alloc, x, y))
        return b.zone;
    }
    return HitZone::TitleBar;
  }

  [[nodiscard]] int DoubleClickTimeMs() override {
    if (!GTK_IS_WIDGET(window_))
      return 400;
    gint ms = 400;
    g_object_get(gtk_widget_get_settings(window_), "gtk-double-click-time", &ms,
                 nullptr);
    return ms;
  }

  [[nodiscard]] int DragThreshold() override {
    if (!GTK_IS_WIDGET(window_))
      return 8;
    gint px = 8;
    g_object_get(gtk_widget_get_settings(window_), "gtk-dnd-drag-threshold",
                 &px, nullptr);
    return px;
  }

  void Dispatch() override {
    // Drain GLib without blocking.  GTK's machinery runs here, so a theme or
    // settings change is observed on the next redraw -- no GMainLoop, and no
    // integration of GLib's fds into the caller's poll set.
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
  }

 private:
  GtkWidget* window_ = nullptr;
  GtkWidget* header_ = nullptr;
  std::string title_;
  InputState state_;
  int laid_out_width_ = -1;

  /// Push focus/maximized state into the widget tree so the theme picks the
  /// matching styling itself.
  void ApplyWindowState() {
    if (state_.focused)
      gtk_widget_unset_state_flags(window_, GTK_STATE_FLAG_BACKDROP);
    else
      gtk_widget_set_state_flags(window_, GTK_STATE_FLAG_BACKDROP, TRUE);

    GtkStyleContext* ctx = gtk_widget_get_style_context(header_);
    if (state_.maximized)
      gtk_style_context_add_class(ctx, "maximized");
    else
      gtk_style_context_remove_class(ctx, "maximized");
  }

  /// Show and size-allocate the header to @p width.
  ///
  /// Unconditional, and must stay that way.  GTK rebuilds the header's buttons
  /// when the decoration layout changes, and the widgets it creates have
  /// neither been shown nor allocated: skipping this because the width happens
  /// to be unchanged leaves them with no geometry, so they draw nothing and
  /// hit-test nothing — the title bar silently loses its buttons.
  void Layout(int width) {
    gtk_widget_show_all(window_);
    GtkAllocation alloc{0, 0, width, 0};
    gtk_widget_get_preferred_height(header_, nullptr, &alloc.height);
    gtk_widget_size_allocate(header_, &alloc);
    laid_out_width_ = width;
  }

  /// Lay out only if the geometry would otherwise be missing or stale.
  ///
  /// For the hit-test path, where a full re-layout per pointer motion would be
  /// waste: the draw path lays out every frame regardless, so a widget-tree
  /// change is picked up there within one frame.
  void LayoutIfNeeded(int width) {
    if (width != laid_out_width_)
      Layout(width);
  }

  void DrawBackground(cairo_t* cr) {
    GtkAllocation alloc{};
    gtk_widget_get_allocation(header_, &alloc);
    gtk_render_background(gtk_widget_get_style_context(header_), cr, alloc.x,
                          alloc.y, alloc.width, alloc.height);
  }

  /// Draw the real GtkLabel, so the title picks up the theme's font and color
  /// rather than a hardcoded family and size.
  void DrawTitle(cairo_surface_t* surface) {
    GtkWidget* label = FindElement(header_, kSelTitle, GTK_TYPE_LABEL).widget;
    if (label == nullptr)
      return;

    GtkAllocation alloc{};
    gtk_widget_get_allocation(label, &alloc);
    if (alloc.width <= 0 || alloc.height <= 0)
      return;

    // Draw into a sub-rectangle so the label's own origin lands where the
    // header laid it out.
    cairo_surface_t* sub = cairo_surface_create_for_rectangle(
        surface, alloc.x, alloc.y, alloc.width, alloc.height);
    cairo_t* cr = cairo_create(sub);
    gtk_widget_size_allocate(label, &alloc);
    gtk_widget_draw(label, cr);
    cairo_destroy(cr);
    cairo_surface_destroy(sub);
  }

  /// The state flags this button should be drawn in, given window state and
  /// where the pointer is.
  [[nodiscard]] GtkStateFlags ButtonState(GtkWidget* button) const {
    auto flags = static_cast<GtkStateFlags>(GTK_STATE_FLAG_NORMAL);
    if (!state_.focused)
      flags = static_cast<GtkStateFlags>(flags | GTK_STATE_FLAG_BACKDROP);

    if (state_.pointer_x >= 0 && state_.pointer_y >= 0) {
      GtkAllocation alloc{};
      gtk_widget_get_clip(button, &alloc);
      if (InRect(alloc, state_.pointer_x, state_.pointer_y)) {
        flags = static_cast<GtkStateFlags>(flags | GTK_STATE_FLAG_PRELIGHT);
        if (state_.pressed)
          flags = static_cast<GtkStateFlags>(flags | GTK_STATE_FLAG_ACTIVE);
      }
    }
    return flags;
  }

  void DrawButton(cairo_t* cr,
                  cairo_surface_t* surface,
                  const char* selector,
                  const char* icon_name) {
    GtkWidget* button = FindElement(header_, selector, GTK_TYPE_BUTTON).widget;
    if (button == nullptr)
      return;  // This layout has no such button.

    GtkStyleContext* style = gtk_widget_get_style_context(button);
    const GtkStateFlags flags = ButtonState(button);

    GtkAllocation alloc{};
    gtk_widget_get_clip(button, &alloc);

    // Background and frame: the theme's own button rendering, in the state we
    // just computed.
    gtk_style_context_save(style);
    gtk_style_context_set_state(style, flags);
    gtk_render_background(style, cr, alloc.x, alloc.y, alloc.width,
                          alloc.height);
    gtk_render_frame(style, cr, alloc.x, alloc.y, alloc.width, alloc.height);
    gtk_style_context_restore(style);

    // Symbolic icon from the icon theme, recolored for the button's state.
    double sx = 1.0;
    double sy = 1.0;
    cairo_surface_get_device_scale(surface, &sx, &sy);
    const auto scale = static_cast<int>((sx + sy) / 2.0);

    gint icon_w = 16;
    gint icon_h = 16;
    if (!gtk_icon_size_lookup(GTK_ICON_SIZE_MENU, &icon_w, &icon_h)) {
      icon_w = 16;
      icon_h = 16;
    }

    GtkIconInfo* info = gtk_icon_theme_lookup_icon_for_scale(
        gtk_icon_theme_get_default(), icon_name, icon_w, scale > 0 ? scale : 1,
        static_cast<GtkIconLookupFlags>(0));
    if (info == nullptr)
      return;  // Icon theme lacks it; the button still drew its background.

    gtk_style_context_save(style);
    gtk_style_context_set_state(style, flags);
    GdkPixbuf* pixbuf =
        gtk_icon_info_load_symbolic_for_context(info, style, nullptr, nullptr);
    gtk_style_context_restore(style);
    g_object_unref(info);
    if (pixbuf == nullptr)
      return;

    cairo_surface_t* icon = gdk_cairo_surface_create_from_pixbuf(
        pixbuf, scale > 0 ? scale : 1, nullptr);
    if (icon != nullptr) {
      GtkWidget* icon_widget = gtk_bin_get_child(GTK_BIN(button));
      gtk_render_icon_surface(icon_widget != nullptr
                                  ? gtk_widget_get_style_context(icon_widget)
                                  : style,
                              cr, icon, alloc.x + ((alloc.width - icon_w) / 2),
                              alloc.y + ((alloc.height - icon_h) / 2));
      cairo_surface_destroy(icon);
    }
    g_object_unref(pixbuf);
  }

  void DrawButtons(cairo_t* cr, cairo_surface_t* surface) {
    // Whichever of these GTK actually built for the active decoration layout
    // gets drawn; the rest are simply absent.
    DrawButton(cr, surface, kSelMinimize, "window-minimize-symbolic");
    DrawButton(cr, surface, kSelMaximize,
               state_.maximized ? "window-restore-symbolic"
                                : "window-maximize-symbolic");
    DrawButton(cr, surface, kSelClose, "window-close-symbolic");
  }
};

}  // namespace

std::unique_ptr<GtkThemeBackend> MakeGtk3Backend() {
  return std::make_unique<Gtk3Backend>();
}

}  // namespace wl::csd::detail

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,
//           cppcoreguidelines-pro-bounds-pointer-arithmetic,
//           cppcoreguidelines-avoid-non-const-global-variables)
