// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// ext-data-control — a focus-free clipboard CLI (copy / paste) over
// ext-data-control-v1.
//
// Unlike the windowed `clipboard` example (core wl_data_device, which is
// delivered only to the client with keyboard focus and whose set_selection
// needs a real input serial), ext-data-control lets any client read and set the
// selection with no surface and no focus at all.  So this is a headless CLI in
// the shape of wl-copy / wl-paste:
//
//   ext-data-control --paste [--mime TYPE] [--list]
//       Write the current selection to stdout.  --list instead prints the
//       offered MIME types, one per line.  With no --mime a text flavor is
//       chosen automatically.
//
//   ext-data-control --copy [--mime TYPE] [--] [TEXT ...]
//       Take the selection.  The data is the TEXT arguments (joined with a
//       space) or, if none are given, everything read from stdin.  The process
//       stays in the foreground serving the data on demand until another client
//       takes the selection or it is interrupted (Ctrl-C).
//
// The entire clipboard mechanism is wl::DataDevice<App,
// wl::ExtDataControlProtocol> — the same helper the core clipboard uses,
// re-parameterized onto the ext-data-control trait bundle.  Only a wl_seat
// (bound without listening to any input) is needed, because get_data_device is
// seat-scoped.
//
// Build requirements: wayland-client, wayland-protocols (>= the staging
// ext-data-control protocol), wayland-scanner.
// Runtime requirement: a compositor that implements ext_data_control_manager_v1
// (wlroots-based compositors, KWin, Mutter, …).

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "ext_data_control_client.hpp"  // ext_data_control_v1::client
#include "wayland_client.hpp"           // wayland::client

// ── Framework headers
// ─────────────────────────────────────────────────────────
#include <wl/client_helpers.hpp>    // wl::BindHandler
#include <wl/data_device.hpp>       // wl::DataDevice, wl::MimeSet
#include <wl/display.hpp>           // wl::Display, wl::RunEventLoop, Roundtrip…
#include <wl/ext_data_control.hpp>  // wl::ExtDataControlProtocol
#include <wl/fd_handle.hpp>         // wl::FdHandle
#include <wl/registry.hpp>          // wl::CRegistry
#include <wl/wl_ptr.hpp>

// ── System headers
// ────────────────────────────────────────────────────────────
extern "C" {
#include <poll.h>
#include <unistd.h>
#include <wayland-client-protocol.h>  // wl_seat_interface
}

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// wl_iface() — the core Wayland interfaces this example binds.
// ══════════════════════════════════════════════════════════════════════════════

namespace wayland::client {

const wl_interface& wl_seat_traits::wl_iface() noexcept {
  return wl_seat_interface;
}

}  // namespace wayland::client

namespace {

// Preferred text MIME types, most specific first, for the automatic flavor
// choice when the caller does not pass --mime.
constexpr std::array<const char*, 5> kTextMimes = {
    "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"};

// The read from a peer's selection is bounded so a stuck source cannot wedge
// the paste; a well-behaved compositor answers immediately.
constexpr int kReadTimeoutMs = 2000;

// SIGINT/SIGTERM stop the --copy serving loop cleanly.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_running = 1;
extern "C" void OnSignal(int /*signo*/) noexcept {
  g_running = 0;
}

enum class Mode { kNone, kCopy, kPaste };

struct Options {
  Mode mode = Mode::kNone;
  const char* mime = nullptr;  // explicit --mime, or nullptr for auto
  bool list = false;           // --list (paste): print offered types
  std::string payload;         // --copy data, when given as TEXT arguments
  bool payload_from_args = false;
};

// A wl_seat we bind but never listen to: ext-data-control is focus-free, so the
// seat is just the object get_data_device is scoped to.
class WlSeatHandler : public wayland::client::CWlSeat<WlSeatHandler> {};

// ══════════════════════════════════════════════════════════════════════════════
// App
// ══════════════════════════════════════════════════════════════════════════════

class App {
 public:
  explicit App(Options opts) noexcept : opts_(std::move(opts)) {}

  ~App() noexcept {
    // Tear down protocol objects before the display drops (see Release order in
    // the other examples): the data device sends its destroy destructor.
    data_device_.Release();
    seat_.Reset();
  }

  App(const App&) = delete;
  App& operator=(const App&) = delete;
  App(App&&) = delete;
  App& operator=(App&&) = delete;

  int Run() {
    if (!ConnectDisplay() || !ScanGlobals() || !BindGlobals())
      return EXIT_FAILURE;
    return opts_.mode == Mode::kCopy ? RunCopy() : RunPaste();
  }

  // ── wl::DataDevice hooks (detected via SFINAE) ────────────────────────────
  void OnSelection(const wl::MimeSet& mimes) {
    // Record the offered flavors for the paste path.  In --copy this also fires
    // for our own selection; we simply don't read it back.
    offered_.assign(mimes.begin(), mimes.end());
    selection_seen_ = true;
  }

  void OnSend(const char* /*mime*/, wl::FdHandle fd) {
    // A peer is pasting our selection: stream the payload into the pipe.  fd
    // closes with the FdHandle, giving the reader EOF.
    const char* p = payload_.data();
    std::size_t left = payload_.size();
    while (left > 0) {
      const ssize_t n = write(fd.Get(), p, left);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;  // peer closed early (EPIPE) or other error
      }
      p += static_cast<std::size_t>(n);
      left -= static_cast<std::size_t>(n);
    }
    ++served_;
  }

  void OnCancelled() {
    // Another client took the selection; our source is gone, so stop serving.
    g_running = 0;
    cancelled_ = true;
  }

 private:
  bool ConnectDisplay() {
    if (!display_.Connect()) {
      std::fprintf(stderr,
                   "ext-data-control: cannot connect to a Wayland "
                   "display (is WAYLAND_DISPLAY set?)\n");
      return false;
    }
    return true;
  }

  bool ScanGlobals() {
    if (!registry_.Create(display_.Get())) {
      std::fprintf(stderr,
                   "ext-data-control: wl_display_get_registry failed\n");
      return false;
    }
    registry_.OnGlobal([this](wl::CRegistry& /*reg*/, std::uint32_t name,
                              std::string_view iface, std::uint32_t ver) {
      using wl_seat = wayland::client::wl_seat_traits;
      using ext_mgr =
          ext_data_control_v1::client::ext_data_control_manager_v1_traits;
      if (iface == wl_seat::interface_name) {
        seat_name_ = name;
        seat_ver_ = ver;
      } else if (iface == ext_mgr::interface_name) {
        data_device_.Record(name, ver);
        have_manager_ = true;
      }
    });
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      std::fprintf(stderr, "ext-data-control: timed out reading globals\n");
      return false;
    }
    if (!have_manager_) {
      std::fprintf(stderr,
                   "ext-data-control: the compositor does not implement "
                   "ext_data_control_manager_v1\n");
      return false;
    }
    if (!seat_name_) {
      std::fprintf(stderr, "ext-data-control: no wl_seat advertised\n");
      return false;
    }
    return true;
  }

  bool BindGlobals() {
    using wayland::client::wl_seat_traits;
    // A version-1 seat is all get_data_device needs; binding low keeps the seat
    // free of the since-5 `release` destructor semantics we would never use.
    if (!wl::BindHandler<wl_seat_traits>(registry_, seat_, seat_name_, 1)) {
      std::fprintf(stderr, "ext-data-control: wl_seat bind failed\n");
      return false;
    }
    if (!data_device_.Bind(registry_, this)) {
      std::fprintf(stderr,
                   "ext-data-control: ext_data_control_manager bind failed\n");
      return false;
    }
    data_device_.Start(display_.Get(), seat_.Get()->GetProxy());
    // One roundtrip so the compositor delivers the current selection (a
    // data_offer, its offer events, and the selection event) to our new device.
    if (!wl::RoundtripWithTimeout(display_.Get())) {
      std::fprintf(stderr,
                   "ext-data-control: timed out starting data device\n");
      return false;
    }
    return true;
  }

  // ── Paste ──────────────────────────────────────────────────────────────────
  int RunPaste() {
    if (!data_device_.HasSelection()) {
      std::fprintf(stderr, "ext-data-control: the clipboard is empty\n");
      return EXIT_FAILURE;
    }
    if (opts_.list) {
      for (const std::string& m : offered_)
        std::printf("%s\n", m.c_str());
      return EXIT_SUCCESS;
    }
    const char* mime = ChooseReadMime();
    if (mime == nullptr) {
      if (opts_.mime != nullptr)
        std::fprintf(stderr,
                     "ext-data-control: the selection does not offer '%s' "
                     "(try --list)\n",
                     opts_.mime);
      else
        std::fprintf(stderr,
                     "ext-data-control: the selection offers no text "
                     "flavor (try --mime TYPE or --list)\n");
      return EXIT_FAILURE;
    }
    wl::FdHandle fd = data_device_.Receive(mime);
    if (fd.Get() < 0) {
      std::fprintf(stderr, "ext-data-control: receive failed\n");
      return EXIT_FAILURE;
    }
    return CopyFdToStdout(fd.Get()) ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  // The MIME type to receive: the explicit --mime when offered, else the first
  // preferred text flavor the selection carries.  nullptr if neither applies.
  [[nodiscard]] const char* ChooseReadMime() const noexcept {
    if (opts_.mime != nullptr) {
      for (const std::string& m : offered_)
        if (m == opts_.mime)
          return opts_.mime;
      return nullptr;
    }
    for (const char* want : kTextMimes)
      for (const std::string& m : offered_)
        if (m == want)
          return want;
    return nullptr;
  }

  // Drain the read end to EOF onto stdout.  Bounded per-read so a stuck source
  // cannot block forever.
  [[nodiscard]] static bool CopyFdToStdout(int fd) noexcept {
    std::array<char, 4096> buf{};
    for (;;) {
      pollfd pfd{fd, POLLIN, 0};
      const int pr = poll(&pfd, 1, kReadTimeoutMs);
      if (pr < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
      if (pr == 0)
        return false;  // timed out waiting for the source
      const ssize_t n = read(fd, buf.data(), buf.size());
      if (n < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
      if (n == 0)
        return true;  // EOF: complete
      std::size_t off = 0;
      while (off < static_cast<std::size_t>(n)) {
        const ssize_t w = write(STDOUT_FILENO, buf.data() + off,
                                static_cast<std::size_t>(n) - off);
        if (w < 0) {
          if (errno == EINTR)
            continue;
          return false;
        }
        off += static_cast<std::size_t>(w);
      }
    }
  }

  // ── Copy ───────────────────────────────────────────────────────────────────
  int RunCopy() {
    payload_ = opts_.payload_from_args ? opts_.payload : ReadAllStdin();

    wl::MimeSet mimes;
    if (opts_.mime != nullptr) {
      mimes.Add(opts_.mime);  // caller pinned an exact flavor (e.g. a binary)
    } else {
      for (const char* m : kTextMimes)
        mimes.Add(m);
    }
    data_device_.Offer(mimes,
                       0);  // serial ignored: set_selection is focus-free

    // Serve OnSend until a peer supersedes us (OnCancelled) or we are signaled.
    const bool ok = wl::RunEventLoop(
        display_.Get(), [] { return g_running == 0; }, "ext-data-control");
    std::fprintf(
        stderr,
        "ext-data-control: %s after serving the selection %llu time%s\n",
        cancelled_ ? "superseded by another client" : "interrupted",
        static_cast<unsigned long long>(served_), served_ == 1 ? "" : "s");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  [[nodiscard]] static std::string ReadAllStdin() {
    std::string out;
    std::array<char, 4096> buf{};
    for (;;) {
      const ssize_t n = read(STDIN_FILENO, buf.data(), buf.size());
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (n == 0)
        break;
      out.append(buf.data(), static_cast<std::size_t>(n));
    }
    return out;
  }

  // ── Members ──────────────────────────────────────────────────────────────
  Options opts_;
  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::WlPtr<WlSeatHandler> seat_;
  wl::DataDevice<App, wl::ExtDataControlProtocol> data_device_;

  std::vector<std::string> offered_;  // MIME types of the current selection
  std::string payload_;               // data served in --copy
  std::uint32_t seat_name_ = 0;
  std::uint32_t seat_ver_ = 0;
  std::uint64_t served_ = 0;
  bool have_manager_ = false;
  bool selection_seen_ = false;
  bool cancelled_ = false;
};

// ══════════════════════════════════════════════════════════════════════════════
// Argument parsing
// ══════════════════════════════════════════════════════════════════════════════

void Usage(const char* argv0) {
  std::fprintf(
      stderr,
      "usage:\n"
      "  %s --paste [--mime TYPE] [--list]\n"
      "  %s --copy  [--mime TYPE] [--] [TEXT ...]\n"
      "\n"
      "  --paste            write the current selection to stdout\n"
      "  --copy             take the selection; data from TEXT args "
      "or stdin\n"
      "  --mime TYPE        MIME type to receive/offer "
      "(default: a text flavor)\n"
      "  --list             (paste) print offered MIME types instead\n",
      argv0, argv0);
}

// Returns false on a usage error (message already printed).
bool ParseArgs(int argc, char** argv, Options& out) {
  int i = 1;
  for (; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--copy") {
      out.mode = Mode::kCopy;
    } else if (a == "--paste") {
      out.mode = Mode::kPaste;
    } else if (a == "--list") {
      out.list = true;
    } else if (a == "--mime") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "ext-data-control: --mime needs an argument\n");
        return false;
      }
      out.mime = argv[++i];
    } else if (a == "--") {
      ++i;
      break;  // remaining are positional TEXT
    } else if (!a.empty() && a.front() == '-' && a != "-") {
      std::fprintf(stderr, "ext-data-control: unknown option '%.*s'\n",
                   static_cast<int>(a.size()), a.data());
      return false;
    } else {
      break;  // first positional TEXT
    }
  }
  // Any remaining arguments are --copy TEXT, joined with a single space.
  for (; i < argc; ++i) {
    if (!out.payload.empty())
      out.payload.push_back(' ');
    out.payload += argv[i];
    out.payload_from_args = true;
  }
  if (out.mode == Mode::kNone) {
    std::fprintf(stderr,
                 "ext-data-control: one of --copy or --paste is "
                 "required\n");
    return false;
  }
  if (out.mode == Mode::kPaste && out.payload_from_args) {
    std::fprintf(stderr,
                 "ext-data-control: TEXT arguments are only valid with "
                 "--copy\n");
    return false;
  }
  if (out.mode == Mode::kCopy && out.list) {
    std::fprintf(stderr,
                 "ext-data-control: --list is only valid with --paste\n");
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseArgs(argc, argv, opts)) {
    Usage(argv[0]);
    return EXIT_FAILURE;
  }

  // A peer that closes the paste pipe early would otherwise deliver SIGPIPE
  // during OnSend's write(); treat it as EPIPE instead.
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  App app(std::move(opts));
  return app.Run();
}
