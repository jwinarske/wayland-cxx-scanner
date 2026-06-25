<!-- SPDX-License-Identifier: MIT -->
# shadertoy example

Run a Shadertoy "Image" shader on Wayland. Two variants share the
platform-agnostic `shadertoy-cxx` renderer:

- **`shadertoy_egl`** — EGL / OpenGL ES 3 (this guide)
- **`shadertoy_vulkan`** — Vulkan (runtime GLSL→SPIR-V)

A shader is a Shadertoy export `.json` (multi-pass) or a bare Image `.frag`.

## CLI

```
shadertoy_egl [--cycle N] [--media DIR] [--blacklist FILE]
              [--audio|--no-audio] [shader.json|shader.frag ...]
```

| Flag | Meaning |
|---|---|
| `--cycle N` | Auto-advance every N seconds (default 0 = off; SPACE/→/← to switch). With no shader args, cycles the bundled set. |
| `--media DIR` | Resolve Shadertoy texture/cubemap `src` under DIR (also `$SHADERTOY_MEDIA_DIR`). The renderer joins `DIR/<basename>`, so DIR must hold the media files directly. |
| `--blacklist FILE` | Skip shader ids (filename stems) listed in FILE, one per line (`#` comments). |
| `--audio` / `--no-audio` | Enable (default) / disable microphone capture for audio (`music`/`mic`) channels. |

Audio channels are fed by the compiled capture back-end (PipeWire or ALSA).
Override selection with `SHADERTOY_AUDIO_BACKEND=pipewire|alsa|none`; pick the
ALSA capture device with `SHADERTOY_ALSA_DEVICE` (default `default`).

## Build (host)

Examples are off by default:

```sh
meson setup build -Dexamples=true
ninja -C build examples/shadertoy/shadertoy_egl
./build/examples/shadertoy/shadertoy_egl ~/shadertoy/<id>.json
```

The audio back-ends are auto-detected: `libpipewire-0.3` and/or `libasound2`
present at build time → mic capture available.

## Run on a device (Raspberry Pi, cross-compiled)

The repo ships emb manifests in `.emb/` (board targets `rpi5-trixie`,
`rpi5-bookworm`, `rpi4-*`). Cross-build from the repo root:

```sh
emb cross . --target rpi5-trixie --build
```

This produces an aarch64 `shadertoy_egl` plus `libshadertoy.so.0` under
`.config/flutter_workspace/cross-build-*/build-egl/`. ALSA capture is included
because the manifest adds `libasound2-dev` to the sysroot.

### Deploy

```sh
B=.config/flutter_workspace/cross-build-*/build-egl
scp $B/examples/shadertoy/shadertoy_egl          pi:~/shadertoy_egl
scp $B/subprojects/shadertoy-cxx/libshadertoy.so.0.3.0  pi:~/lib/libshadertoy.so.0
scp ~/shadertoy/*.json                            pi:~/shadertoy/
scp /path/to/media/*                              pi:~/shadertoy-media/   # flat files
```

`shadertoy_egl` is a Wayland client, so the device needs a compositor.

### Headless (offscreen, no monitor)

```sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
weston --backend=headless --renderer=gl --width=1280 --height=720 --socket=wayland-9 &
LD_LIBRARY_PATH=~/lib WAYLAND_DISPLAY=wayland-9 \
  ~/shadertoy_egl --media ~/shadertoy-media ~/shadertoy/<id>.json
```

`--renderer=gl` is required so the compositor exposes EGL to the client.

### On the attached display (HDMI, fullscreen)

`cage` is a single-app kiosk compositor (renders straight to DRM/KMS):

```sh
LD_LIBRARY_PATH=~/lib cage -- ~/shadertoy_egl --media ~/shadertoy-media ~/shadertoy/<id>.json
```

Capture a frame under cage (wlroots) with `grim`:

```sh
WAYLAND_DISPLAY=<cage-socket> grim frame.png
```

### Audio on device

List capture devices and pick one that has signal (the system `default` PCM is
often capture-less):

```sh
arecord -L                      # PCM names, e.g. plughw:CARD=CODEC,DEV=0
LD_LIBRARY_PATH=~/lib SHADERTOY_ALSA_DEVICE=plughw:CARD=CODEC,DEV=0 \
  cage -- ~/shadertoy_egl --audio ~/shadertoy/4lGSzy.json
```
