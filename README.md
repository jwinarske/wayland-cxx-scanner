# wayland-cxx-scanner

[![Documentation Status](https://readthedocs.org/projects/wayland-cxx-scanner/badge/?version=latest)](https://wayland-cxx-scanner.readthedocs.io/en/latest/?badge=latest)

A code-generation tool and companion C++23 framework for the
[Wayland](https://wayland.freedesktop.org/) display protocol. Given a Wayland
XML protocol definition it produces type-safe, zero-overhead C++23 headers that
replace the handwritten C bindings normally provided by `wayland-scanner`.

## Features

- **Type-safe protocol bindings** — CRTP proxy and resource classes with
  compile-time event/request dispatch (no `void*` casts in user code).
- **Zero-overhead event dispatch** — generated callbacks use direct CRTP
  `static_cast` dispatch, identical in cost to raw C `wayland-scanner` output
  (no intermediate opcode scan, no `void*` packing/unpacking).
- **Multiple output modes** — client C++ header, server C++ header, and
  C-compatible header from a single XML source.
- **C++ standard selection** — generate code targeting C++17, C++20, or C++23
  (`--std` flag).
- **Header-only framework** — the `wayland-cxx` library installs to
  `${includedir}/wl/` and requires no link-time dependencies of its own.
- **Type-safe object construction** — `wl::construct<ChildTraits, Opcode>()`
  and `wl::construct_at_end<>()` encode interface type and opcode at compile
  time, replacing error-prone `_MarshalNew` calls.
- **RAII everywhere** — `WlPtr<T>` (owning proxy wrapper), `FdHandle` (file
  descriptor), `FileHandle` (C FILE\*), `ScopeExit` (scope guard).
- **Keyboard repeat** — `KeyboardHandler<App>` provides full xkbcommon keymap
  processing with POSIX-timer-based key repeat out of the box.
- **Client-side decorations** — pluggable CSD framework with Cairo and GTK
  back-ends.
- **Strict quality gates** — CI enforces `-Werror`, clang-tidy, clang-format,
  and CodeQL on every push.

## Getting Started

### Prerequisites

| Tool / Library                                                                | Minimum             | Notes                                          |
|-------------------------------------------------------------------------------|---------------------|------------------------------------------------|
| [Meson](https://mesonbuild.com/)                                              | 1.1                 | Build system                                   |
| C++23 compiler                                                                | GCC 13+ / Clang 17+ | `c++23` is the default standard                |
| [pugixml](https://pugixml.org/)                                               | —                   | Auto-fetched via Meson wrap if missing         |
| wayland-client / wayland-server                                               | —                   | Optional; required for examples                |
| [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) | —                   | Optional; required for some tests and examples |
| [Google Test](https://github.com/google/googletest)                           | —                   | Optional; required for tests (auto-fetched)    |
| [libxkbcommon](https://xkbcommon.org/)                                        | —                   | Optional; required for keyboard examples/tests |
| [SDL3](https://github.com/libsdl-org/SDL)                                     | 3.x                 | Optional; required for `sdl3-presentation-shm` |

**Ubuntu / Debian:**

```sh
sudo apt install meson ninja-build libpugixml-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  libgtest-dev
```

**Fedora:**

```sh
sudo dnf install meson ninja-build pugixml-devel \
  wayland-devel wayland-protocols-devel libxkbcommon-devel \
  gtest-devel
```

### Building

```sh
# Clone the repository
git clone https://github.com/jwinarske/wayland-cxx-scanner.git
cd wayland-cxx-scanner

# Configure (scanner only)
meson setup build

# Build
ninja -C build
```

### Building with examples

Most examples require `wayland-client`, `wayland-protocols`, and their
protocol-specific dependencies (see the [Examples](#examples) table below).

```sh
# Configure with examples enabled
meson setup build -Dexamples=true

# Build everything
ninja -C build

# Or build a single example
ninja -C build examples/presentation-shm/presentation_shm
ninja -C build examples/sdl3-presentation-shm/sdl3_presentation_shm
```

### Building with tests

```sh
meson setup build -Dtests=true
ninja -C build
meson test -C build
```

### Installation

```sh
meson setup build --prefix=/usr/local
ninja -C build install
```

This installs:

- `wayland-cxx-scanner` — the code-generation tool.
- `${includedir}/wl/*.hpp` — the framework headers.
- `wayland-cxx.pc` — pkg-config file for downstream consumers.

## Usage

```
wayland-cxx-scanner [--mode=<mode>] [--std=<std>] <protocol.xml> [<output.hpp>]
```

| Flag     | Values                                       | Default         | Description                |
|----------|----------------------------------------------|-----------------|----------------------------|
| `--mode` | `client-header`, `server-header`, `c-header` | `client-header` | Kind of header to generate |
| `--std`  | `c++17`, `c++20`, `c++23`                    | `c++23`         | Target C++ standard        |

If `<output.hpp>` is omitted the generated code is written to `stdout`.

### Example: generate a client header from the XDG shell protocol

```sh
wayland-cxx-scanner \
  --mode=client-header \
  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
  xdg_shell_client.hpp
```

### Meson integration

The scanner integrates into Meson builds with `custom_target` or `generator`.
Every example under `examples/` demonstrates this pattern — see
[examples/minimal/meson.build](examples/minimal/meson.build) for the simplest
case.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full design document. A brief
overview follows.

### Pipeline

```
  ┌───────────────┐      ┌──────────────────┐      ┌─────────────────────┐
  │  protocol.xml │────▶│   XML Parser     │────▶│  Intermediate       │
  │  (Wayland)    │      │  (xml_parser)    │      │  Representation     │
  └───────────────┘      └──────────────────┘      │  (ir.hpp)           │
                                                   └────────┬────────────┘
                                                            │
                             ┌──────────────────────────────┼──────────────────┐
                             │                              │                  │
                             ▼                              ▼                  ▼
                   ┌──────────────────┐        ┌──────────────────┐  ┌──────────────────┐
                   │ codegen_client   │        │ codegen_server   │  │  codegen_c       │
                   │ _cxx             │        │ _cxx             │  │                  │
                   └────────┬─────────┘        └────────┬─────────┘  └────────┬─────────┘
                            │                           │                     │
                            ▼                           ▼                     ▼
                   client proxy .hpp          server resource .hpp     C-style .h
```

### Repository layout

```
wayland-cxx-scanner/
├── include/wl/          Framework headers (installed as wayland-cxx)
├── src/                 Scanner tool source code
├── protocols/           Bundled Wayland XML protocol definitions
├── tests/               Unit and integration tests (Google Test)
├── examples/            Example Wayland client/server applications
├── subprojects/         Meson wrap files (pugixml, gtest)
├── scripts/             Developer tooling (format.sh)
└── meta-wayland-cxx-scanner/  Yocto / OpenEmbedded recipe
```

### Framework headers (`include/wl/`)

| Header               | Purpose                                                                           |
|----------------------|-----------------------------------------------------------------------------------|
| `proxy.hpp`          | Non-owning `CProxy<Traits>` handle (≈ WTL `CWindow`)                              |
| `proxy_impl.hpp`     | CRTP `CProxyImpl<Derived, Traits>` + `wl::construct<>` / `wl::construct_at_end<>` |
| `resource_impl.hpp`  | Server-side `CResourceImpl<Derived, Traits>`                                      |
| `event_map.hpp`      | Optional `CEventMap` base + `BEGIN_EVENT_MAP` macros (for hand-written code)      |
| `wl_ptr.hpp`         | Owning `WlPtr<T>` (≈ WTL `CAutoPtr`)                                              |
| `registry.hpp`       | `CRegistry` client registry + `CGlobal<Traits>` server global factory             |
| `display.hpp`        | `CDisplay` RAII display wrapper + `RunEventLoop()`                                |
| `fd_handle.hpp`      | RAII file descriptor wrapper                                                      |
| `raii.hpp`           | `FileHandle` (C FILE\*) + `ScopeExit` scope guard                                 |
| `client_helpers.hpp` | `SetupHandler()` / `BindHandler()` convenience functions                          |
| `keyboard.hpp`       | `KeyboardHandler<App>` — xkbcommon keymap + key repeat                            |
| `seat.hpp`           | `SeatManager<App>` — seat capability tracking                                     |
| `xdg_shell.hpp`      | Pre-built wl\_interface tables for XDG shell                                      |
| `xdg_decoration.hpp` | XDG decoration protocol support                                                   |
| `linux_dmabuf.hpp`   | Linux DMA-BUF protocol support                                                    |
| `agl_shell.hpp`      | AGL (Automotive Grade Linux) shell protocol support                               |
| `csd_plugin.hpp`     | Client-side decoration plugin interface                                           |
| `csd_fallback.hpp`   | Fallback CSD implementation                                                       |
| `csd_cairo.hpp`      | Cairo-based CSD implementation                                                    |
| `csd_gtk.hpp`        | GTK-based CSD implementation                                                      |
| `wayland.hpp`        | Umbrella include for common framework headers                                     |

## Examples

All examples live under `examples/` and are built when `-Dexamples=true` is
passed to `meson setup` (requires `wayland-client` and `wayland-server`).

| Example                                                       | Description                                      | Extra dependencies           |
|---------------------------------------------------------------|--------------------------------------------------|------------------------------|
| [minimal](examples/minimal)                                   | Client ↔ server roundtrip with a custom protocol | —                            |
| [wayland-info](examples/wayland-info)                         | Print compositor globals and capabilities        | wayland-protocols (optional) |
| [key-input](examples/key-input)                               | Keyboard input handling with xkbcommon           | xkbcommon                    |
| [simple-egl](examples/simple-egl)                             | Animated EGL/GLES triangle                       | EGL, GLESv2                  |
| [subsurfaces](examples/subsurfaces)                           | Subsurface protocol demonstration                | EGL, GLESv2                  |
| [presentation-shm](examples/presentation-shm)                 | Frame-timing feedback via `wp_presentation`      | wayland-protocols            |
| [sdl3-presentation-shm](examples/sdl3-presentation-shm)       | SDL3 window with `wp_presentation` frame-timing  | SDL3, wayland-protocols      |
| [agl-presentation-shm](examples/agl-presentation-shm)         | AGL compositor integration                       | wayland-protocols            |
| [ivi-presentation-shm](examples/ivi-presentation-shm)         | IVI shell integration                            | wayland-protocols            |
| [xdg-csd](examples/xdg-csd)                                   | Client-side decorations (Cairo / GTK back-ends)  | cairo or gtk+-3.0            |
| [xdg-simple-dmabuf-vulkan](examples/xdg-simple-dmabuf-vulkan) | Vulkan rendering with DMA-BUF export             | Vulkan                       |

## Testing

The project uses [Google Test](https://github.com/google/googletest) with
automatic download via Meson wraps when the system package is not found.

```sh
meson setup build -Dtests=true
meson test -C build
```

### Test suite

| Category             | Tests                                                                                         | Timeout |
|----------------------|-----------------------------------------------------------------------------------------------|---------|
| **Unit — scanner**   | ir, xml\_parser, name\_transform, codegen\_c, codegen\_client\_cxx, codegen\_server\_cxx, cli | 15–30 s |
| **Unit — framework** | raii, event\_map, proxy, proxy\_impl, resource\_impl, client\_helpers, display                | 15 s    |
| **Unit — protocols** | agl\_shell, xdg\_shell (optional), seat/keyboard (optional)                                   | 15 s    |
| **Integration**      | roundtrip (client + server in forked processes)                                               | 60 s    |

## Build Options

| Option     | Type    | Default | Description                          |
|------------|---------|---------|--------------------------------------|
| `tests`    | boolean | `false` | Build and run unit/integration tests |
| `examples` | boolean | `false` | Build example applications           |
| `docs`     | boolean | `false` | Generate Doxygen HTML documentation  |

## Code Quality

### clang-format

All C++ sources follow the [Chromium](https://chromium.googlesource.com/chromium/src/+/HEAD/styleguide/c++/c++.md)
formatting style enforced by `clang-format-19`. Run locally:

```sh
scripts/format.sh          # reformat in-place
scripts/format.sh --check  # dry-run (CI mode)
```

### clang-tidy

A comprehensive `.clang-tidy` configuration enables checks from `bugprone-*`,
`cert-*`, `cppcoreguidelines-*`, `modernize-*`, `performance-*`,
`readability-*`, and `clang-analyzer-security.*`. All findings are treated as
errors (`WarningsAsErrors: '*'`).

### CodeQL

GitHub's CodeQL analysis runs on every push and pull request, and weekly on a
schedule. The `security-extended` and `security-and-quality` query suites are
enabled.

## CI Matrix

| Platform        | Compilers                        | Workflow     |
|-----------------|----------------------------------|--------------|
| Ubuntu 24.04    | GCC, Clang 19                    | `ci.yml`     |
| Fedora (latest) | GCC, Clang                       | `ci.yml`     |
| Ubuntu 24.04    | clang-format-19, clang-tidy-19   | `lint.yml`   |
| Ubuntu 24.04    | CodeQL (C/C++)                   | `codeql.yml` |
| Ubuntu 24.04    | Doxygen + Sphinx (Read the Docs) | `docs.yml`   |

## Yocto / OpenEmbedded

A BitBake recipe is provided in `meta-wayland-cxx-scanner/` for integration
into Yocto-based embedded Linux builds.

## Contributing

1. Fork the repository and create a feature branch.
2. Run `scripts/format.sh` before committing.
3. Ensure `meson test -C build` passes with `-Dtests=true`.
4. Open a pull request — CI will verify formatting, clang-tidy, CodeQL, and the
   full test suite.

## License

[MIT](LICENSE) — Copyright © 2026 Joel Winarske
