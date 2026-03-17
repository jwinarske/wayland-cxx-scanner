# Architecture

## Overview

`wayland-cxx-scanner` is a code-generation tool and companion C++23 framework
for the [Wayland](https://wayland.freedesktop.org/) display protocol.  Given a
Wayland XML protocol definition it produces type-safe, zero-overhead C++23
headers that replace the hand-written C bindings normally provided by
`wayland-scanner`.

The project has two distinct deliverables:

| Deliverable | Description |
|---|---|
| **Scanner tool** (`wayland-cxx-scanner`) | Reads a `.xml` protocol file and writes a `.hpp` header. |
| **Framework library** (`wayland-cxx`) | A header-only C++23 base library installed to `${includedir}/wl/`. |

---

## Repository Layout

```
wayland-cxx-scanner/
├── include/wl/          # Framework headers (installed)
├── src/                 # Scanner tool source code
├── protocols/           # Bundled Wayland XML protocol definitions
├── tests/               # Unit and integration tests
├── examples/            # Example Wayland client/server applications
├── subprojects/         # Meson wrap files (e.g. pugixml)
└── meta-wayland-cxx-scanner/  # Yocto/OpenEmbedded recipe metadata
```

---

## Scanner Tool

### Pipeline

```
  ┌──────────────┐      ┌──────────────────┐      ┌─────────────────────┐
  │  protocol.xml │─────▶│   XML Parser     │─────▶│  Intermediate       │
  │  (Wayland)    │      │  (xml_parser)    │      │  Representation     │
  └──────────────┘      └──────────────────┘      │  (ir.hpp)           │
                                                   └────────┬────────────┘
                                                            │
                              ┌─────────────────────────────┼──────────────────┐
                              │                             │                  │
                              ▼                             ▼                  ▼
                   ┌──────────────────┐        ┌──────────────────┐  ┌──────────────────┐
                   │ codegen_client   │        │ codegen_server   │  │  codegen_c       │
                   │ _cxx             │        │ _cxx             │  │                  │
                   └────────┬─────────┘        └────────┬─────────┘  └────────┬─────────┘
                            │                           │                     │
                            ▼                           ▼                     ▼
                   client proxy .hpp          server resource .hpp     C-style .h
```

### Modules

#### `xml_parser` (`src/xml_parser.{cpp,hpp}`)

Parses a Wayland XML protocol document using the
[pugixml](https://pugixml.org/) library.  The two entry points are:

```cpp
Protocol parse_protocol(const std::filesystem::path& path);
Protocol parse_protocol_from_string(std::string_view xml);
```

Both return a fully populated `ir::Protocol` tree or throw `ir::ParseError` on
malformed input.

#### Intermediate Representation (`src/ir.hpp`)

A simple, self-contained data model in the `wl::scanner::ir` namespace:

```
Protocol
└── Interface[]
    ├── Message[]   (requests and events, tagged by opcode)
    │   └── Arg[]   (typed arguments with optional interface/enum qualifiers)
    └── Enum[]
        └── EnumEntry[]
```

Key types:

| Type | Description |
|---|---|
| `ir::Protocol` | Top-level container; holds all parsed interfaces. |
| `ir::Interface` | One `<interface>` block with requests, events, and enums. |
| `ir::Message` | A single `<request>` or `<event>`, carrying its opcode and argument list. |
| `ir::Arg` | One typed argument; `ArgType` maps to the Wayland wire types. |
| `ir::Enum` / `ir::EnumEntry` | Named enumeration with optional bitfield flag. |
| `ir::ParseError` | Exception thrown for malformed XML. |

#### `name_transform` (`src/name_transform.{cpp,hpp}`)

Utility functions that convert Wayland `snake_case` identifiers into C++
`PascalCase` and `camelCase` names used by the generated code.

#### Code Generators

| Module | Generates | Entry point |
|---|---|---|
| `codegen_client_cxx` | C++23 client proxy headers (CRTP, event maps) | `generate_client_cxx_header()` |
| `codegen_server_cxx` | C++23 server resource headers (CRTP, request maps) | `generate_server_cxx_header()` |
| `codegen_c` | C-style protocol headers (compatible with `wayland-scanner` output) | `generate_c_header()` |

All three generators accept a `CppStd` enum (`Cpp17`, `Cpp20`, `Cpp23`) that
gates which language features appear in the output.

#### `main.cpp`

Command-line driver.  Accepts `--mode=<mode>` and `--std=<std>` flags,
delegates to the appropriate generator, and writes the result to a file or
`stdout`.

```
Usage: wayland-cxx-scanner [--mode=<mode>] [--std=<std>] <protocol.xml> [<output.hpp>]

Modes:
  client-header   Generate C++ client proxy header (default)
  server-header   Generate C++ server resource header
  c-header        Generate C-style protocol header
```

---

## Framework Library (`include/wl/`)

The framework is a header-only base library that generated code builds upon.
It follows the WTL (Windows Template Library) message-map pattern adapted for
Wayland events and requests.

### Class Hierarchy

```
wl::CEventMap           (abstract: ProcessEvent)
    └── wl::CProxyImpl<Derived, Traits>   (CRTP client proxy base)
            └── <Generated> CXxxInterface<App>

wl::CProxy<Traits>      (non-owning handle wrapper, ≈ WTL CWindow)
    └── wl::CProxyImpl  (also inherits CProxy)

wl::WlPtr<T>            (RAII owning wrapper, ≈ WTL CAutoPtr)
```

### Key Headers

#### `proxy.hpp` — `wl::CProxy<Traits>`

Non-owning, type-safe handle around a `wl_proxy*`.  Analogous to WTL's
`CWindow`.  The `Traits` template parameter must satisfy the `WlProxyTraits`
concept, requiring:

- `static constexpr std::string_view interface_name`
- `static constexpr uint32_t version`
- `static const wl_interface& wl_iface() noexcept`

#### `event_map.hpp` — `wl::CEventMap` + macros

Abstract base for opcode dispatch.  Generated proxy and resource classes
implement `ProcessEvent` using the macro DSL:

```cpp
BEGIN_EVENT_MAP(MyProxy)
  EVENT_HANDLER(0, OnEnter)
  EVENT_HANDLER(1, OnLeave)
END_EVENT_MAP()
```

Server-side equivalents: `BEGIN_REQUEST_MAP` / `REQUEST_HANDLER` /
`END_REQUEST_MAP`.

#### `proxy_impl.hpp` — `wl::CProxyImpl<Derived, Traits>`

CRTP base that combines `CProxy` and `CEventMap`.  Provides:

- `_SetProxy(wl_proxy*)` — attach and install the static listener table.
- `_Marshal(opcode, args…)` — send a request to the compositor.
- `_MarshalNew(opcode, &iface, args…)` — send a request that creates a new object.

#### `wl_ptr.hpp` — `wl::WlPtr<T>`

RAII owning wrapper for a `CProxy`-derived object.  Calls `T::Destroy()` on
destruction or reset.  Ownership is transferred via `Attach()`/`Detach()` on
the underlying `wl_proxy*` (the deleted copy/move constructors of `CProxy`
prevent naive ownership transfer).

#### `registry.hpp` — `wl::CRegistry` and `wl::CGlobal<Traits>`

- `CRegistry`: client-side registry wrapper.  Exposes typed `OnGlobal` /
  `OnRemove` callbacks and a templated `Bind<Traits>()` helper.
- `CGlobal<Traits>`: server-side global factory that advertises an interface
  on the Wayland display.

#### `display.hpp` — `wl::CDisplay`

RAII wrapper around `wl_display`.  Manages connection lifecycle and the event
loop.

#### `raii.hpp`

Lightweight RAII helpers for raw Wayland C objects not covered by the typed
wrappers.

#### `client_helpers.hpp`

Utility functions and types used by client-side generated code and example
applications.

#### `keyboard.hpp` / `seat.hpp`

Higher-level seat and keyboard input handlers.  `wl::KeyboardHandler<App>`
requires the application class to implement `OnKey(key, state)` and optionally
`OnKeySym(xkb_keysym_t, key, state)`.

#### `wayland.hpp` / `xdg_shell.hpp`

Pre-generated proxy headers for the core Wayland and XDG shell protocols,
provided as part of the installed framework so consumers do not need to run the
scanner themselves for these common protocols.

---

## Build System (Meson)

The project uses [Meson](https://mesonbuild.com/) ≥ 1.1 with C++23.

### Build Options

| Option | Type | Default | Description |
|---|---|---|---|
| `tests` | boolean | `false` | Build and run unit/integration tests |
| `examples` | boolean | `false` | Build example Wayland applications |
| `docs` | boolean | `false` | Generate Doxygen HTML documentation |

### Dependencies

| Dependency | Required | Notes |
|---|---|---|
| `pugixml` | Yes | Bundled as a Meson subproject fallback |
| `wayland-client` | Optional | Required for examples and client headers |
| `wayland-server` | Optional | Required for examples and server headers |

---

## Generated Code Conventions

### Client Side

For each `<interface>` the scanner emits:

1. **Traits struct** — satisfies `WlProxyTraits`; holds `interface_name`,
   `version`, and a reference to the protocol's `wl_interface`.
2. **Proxy class** `CXxx<Derived>` — CRTP template inheriting
   `wl::CProxyImpl<Derived, XxxTraits>`.  One `_EvtN` static function per
   event; one `SendFoo()` method per request.
3. **Listener table** `s_listener_table_` — `inline static const void*[]`
   array of `_EvtN` function pointers passed to `wl_proxy_add_listener`.

### Server Side

For each `<interface>` the scanner emits:

1. **Traits struct** — server-side interface traits.
2. **Resource class** `CXxx<Derived>` — CRTP template inheriting
   `wl::CResourceImpl<Derived, XxxTraits>` with `_ReqN` static functions and
   `SendFoo()` event senders.
3. **Dispatch table** for `wl_resource_set_implementation`.

### Opcode Macros

`EVENT_HANDLER` and `REQUEST_HANDLER` macros require raw integer opcodes (not
symbolic names) because the `##` token-paste operator must produce single
identifiers such as `_CrackEvent_0`.

---

## Data Flow Example

```
wayland-cxx-scanner --mode=client-header xdg-shell.xml xdg_shell.hpp
                          │
          ┌───────────────▼────────────────┐
          │   parse_protocol("xdg-shell.xml")  │
          │   → ir::Protocol               │
          └───────────────┬────────────────┘
                          │
          ┌───────────────▼────────────────┐
          │  generate_client_cxx_header()  │
          │  (codegen_client_cxx.cpp)      │
          └───────────────┬────────────────┘
                          │
                    xdg_shell.hpp
                          │
          ┌───────────────▼────────────────┐
          │  Application code              │
          │  #include "xdg_shell.hpp"      │
          │  class App : CXdgSurface<App>  │
          │  { BEGIN_EVENT_MAP(App)        │
          │    EVENT_HANDLER(0, OnConfigure│
          │    END_EVENT_MAP() }           │
          └────────────────────────────────┘
```
