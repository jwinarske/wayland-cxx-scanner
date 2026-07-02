# GN integration

wayland-cxx-scanner ships a GN build (`BUILD.gn` + `wayland_cxx_scanner.gni`)
alongside the meson/CMake builds, so a GN consumer — chiefly Chromium's
`ozone/wayland` — can generate type-safe C++ Wayland **client** bindings and
compile them with the embedder's own clang + libc++. This mirrors drm-cxx's GN
build (used by `ozone/drm`): same-toolchain codegen, no C ABI shim.

## What the GN build provides

| Target | Kind | Purpose |
|--------|------|---------|
| `:wayland_cxx_scanner` | `executable` (host) | the code generator; built + run at build time |
| `:pugixml` | `source_set` | vendored pugixml (`subprojects/pugixml-1.15/`), the scanner's only dep |
| `:wayland_cxx` | `source_set` | header-only `<wl/...>` framework (include config only) |
| `wayland_cxx_protocol(...)` | template (in the `.gni`) | run the scanner on an XML → a C++ header + a target that carries its include dirs |

## Vendoring

Place the tree at `//third_party/wayland-cxx-scanner` (or set
`wayland_cxx_scanner_root` in the `.gni`). `subprojects/pugixml-1.15/` must come
along — Chromium has no pugixml of its own. The `include/wl/` framework and
`src/` scanner sources are the only other required paths.

## Usage

```gn
import("//third_party/wayland-cxx-scanner/wayland_cxx_scanner.gni")

wayland_cxx_protocol("xdg_shell_client") {
  protocol = "//third_party/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
  mode = "client-header"   # or server-header / c-header
  std  = "c++20"           # optional; Chromium builds at c++20
}

source_set("my_wayland_client") {
  deps = [ ":xdg_shell_client" ]   # pulls the generated header dir + <wl/...>
  # ... you link wayland-client yourself (Chromium's ozone/wayland already does)
}
```

The generated header lands at `$target_gen_dir/xdg-shell.hpp` (override with
`output_name`). Depending on the `wayland_cxx_protocol` target puts both that
directory and the framework's `include/` on your compile line.

## Notes / limits

- The scanner is a **host** tool: the template builds
  `:wayland_cxx_scanner($host_toolchain)` and runs it via
  `//build/gn_run_binary.py`. So a Chromium-like `//build` is assumed (compiler
  configs, `gn_run_binary.py`), same as drm-cxx's GN build.
- The `wl/` framework is header-only and carries **no** link-time deps; the
  consumer links `wayland-client` / `wayland-server`.
- The meson and CMake builds are unchanged and remain the standalone path.
