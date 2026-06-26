#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# gen_blacklist.sh — build a shadertoy --blacklist by running each shader and
# recording the ones that hang the GPU (or crash) so a later --cycle run skips
# them.  Output is one shader id (the filename stem) per line, exactly the
# format that `shadertoy_egl --blacklist FILE` consumes (main_egl.cpp).
#
# Hang detection is journal-based: amdgpu prints "ring <name> timeout" on every
# GPU reset, so a shader whose run increases that count wedged the GPU.  A
# severe hang can kill the whole desktop session, so the run is RESUMABLE: an
# in-flight marker survives a crash and the interrupted shader is blacklisted on
# the next invocation.  Just re-run the same command to continue where it left
# off.
#
# WARNING: running an unvetted shader can hang the GPU and take down your
# graphics session.  On a headless/embedded target each shader is hosted in its
# own cage compositor (DRM/KMS); on a desktop, set WAYLAND_DISPLAY to run the
# clients nested in the existing session.
#
# --install registers a systemd *user* service (any systemd distro) that runs
# the scan at boot via linger and is restarted after a crash, resuming from the
# persistent ledger + .cur marker until the whole list is vetted; it removes
# itself on clean completion. Add --watchdog to also install a *root* service
# that reboots the machine (kernel sysrq, fallback systemctl/reboot) when a
# shader hard-hangs the GPU (a D-state process timeout/SIGKILL cannot reap) —
# after the reboot the scan resumes and blacklists the hung shader. Unattended.
# (Persistence note: state must survive reboot, so a read-only/overlay root must
# expose a persistent path via -o and the shader DIR.)
#
# Usage:
#   gen_blacklist.sh [-o OUT] [-d DIR] [-b BIN] [-t SECS] [-m MEDIA]
#   gen_blacklist.sh --install [--watchdog] [same options]  # unattended
#   gen_blacklist.sh --uninstall                            # remove service(s)
#
#   -o, --out FILE     blacklist to write       (default: ./shadertoy.blacklist)
#   -d, --dir DIR      shader directory to scan (default: $SHADERTOY_SHADER_DIR
#                      or ~/shadertoy)
#   -b, --bin PATH     shadertoy_egl binary     (default: autodetect)
#   -t, --secs N       seconds to run each shader before moving on (default: 8)
#   -m, --media DIR    passed through as --media (texture/cubemap dir)
#       --kill-after N grace seconds before SIGKILL (default: 5)
#       --install      install+start a systemd user service (auto-resumes)
#       --watchdog     with --install: add a root reboot-on-hard-hang service
#       --stall N      watchdog stall threshold in seconds (default: 90)
#       --render-check N  sample on-screen rendering every N ok shaders to catch
#                      a frozen display (loop alive but GPU stuck); 0 disables.
#                      Needs grim + cage; flags the watchdog to reboot (default 30)
#       --uninstall    stop and remove the service(s)
#   -h, --help         this help

set -u

# ── Hidden watchdog daemon (run as root by the system watchdog service) ───────
# Kept before the $HOME-dependent defaults so it works in a bare system-service
# environment (no HOME/XDG vars). Watches the scan's .cur marker: if a shader
# stays in-flight (.cur non-empty and unchanged) past the stall threshold — a
# D-state GPU hang that timeout/SIGKILL cannot reap — it forces a reboot. On
# reboot the scan service resumes and the .cur marker blacklists the hung
# shader. Reboot path is portable across systemd distros: a kernel-level sysrq
# reboot (works even when userspace is wedged), falling back to systemctl/reboot
# — no softdog or specific watchdog hardware required.
if [ "${1:-}" = "__watchdog" ]; then
  cur="${2:?usage: __watchdog <cur-file> <stall-secs> [user]}"
  stall="${3:?}"
  user="${4:-}"                    # user whose scan service this guards
  scan_unit="shadertoy-blacklist.service"
  beat="${cur%.cur}.beat"          # heartbeat the scan touches every iteration
  dead="${cur%.cur}.render-dead"   # sentinel the scan drops when rendering froze
  wd_start=$(date +%s)             # only trust a heartbeat refreshed after this
  { echo 1 > /proc/sys/kernel/sysrq; } 2>/dev/null || true   # allow sysrq reboot
  wd_reboot() {
    printf 'watchdog: %s — forcing reboot\n' "$1" >&2
    sync 2>/dev/null || true
    echo b > /proc/sysrq-trigger 2>/dev/null                 # immediate kernel reboot
    systemctl reboot --force 2>/dev/null || reboot -ff 2>/dev/null || true
    exit 0
  }
  # True only when we can confirm the scan service is NOT running. A .cur or
  # render-dead flag left by a stopped/SIGKILLed scan must not reboot a Pi that
  # is meant to be idle — otherwise it boot-loops. Querying systemd (not pgrep)
  # is reliable even during a GPU wedge, where pgrep itself can hang.
  scan_stopped() {
    [ -n "$user" ] || return 1   # unknown user → assume running (recover hangs)
    # Only POSITIVELY-stopped states suppress a reboot; an unknown/empty/error
    # result is treated as "running" so a real hang is still recovered.
    case "$(systemctl --user -M "$user@.host" is-active "$scan_unit" 2>/dev/null)" in
      inactive|failed|deactivating) return 0 ;;
      *) return 1 ;;
    esac
  }
  printf 'watchdog: monitoring %s (stall %ss, user %s)\n' "$cur" "$stall" "${user:-?}" >&2
  iv=$((stall / 3)); [ "$iv" -ge 1 ] || iv=1
  while :; do
    reason=""
    # (1) render-freeze: loop advancing but the GPU stopped putting new frames on
    # screen (display/KMS wedged) — the scan flags this since progress can't.
    [ -f "$dead" ] && reason="scan reported a frozen display"
    # (2) stall: the scan stopped making progress (heartbeat not refreshed) for
    # too long — a hard hang it cannot move past, anywhere in the iteration
    # (including the render-check, where .cur is empty, which the old .cur-only
    # check missed). Only trust a heartbeat touched since this watchdog started,
    # so a stale beat left from a prior boot cannot trigger an instant reboot.
    if [ -z "$reason" ] && [ -f "$beat" ]; then
      bm=$(stat -c %Y "$beat" 2>/dev/null || echo 0)
      if [ "$bm" -ge "$wd_start" ]; then
        bage=$(( $(date +%s) - bm ))
        [ "$bage" -ge "$stall" ] &&
          reason="scan made no progress for ${bage}s (in-flight: \"$(cat "$cur" 2>/dev/null)\")"
      fi
    fi
    [ -n "$reason" ] && ! scan_stopped && wd_reboot "$reason"
    sleep "$iv"
  done
fi

# ── Defaults ──────────────────────────────────────────────────────────────────
out="shadertoy.blacklist"
dir="${SHADERTOY_SHADER_DIR:-$HOME/shadertoy}"
bin=""
secs=8
kill_after=5
media=""
do_install=0
do_uninstall=0
do_watchdog=0
stall_secs=90
render_check_every=30   # sample on-screen rendering every N ok shaders (0=off)

self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
unit="shadertoy-blacklist.service"
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
unit_path="$unit_dir/$unit"
wd_unit="shadertoy-blacklist-watchdog.service"   # system service (root)
wd_unit_path="/etc/systemd/system/$wd_unit"

die() { printf 'gen_blacklist: %s\n' "$*" >&2; exit 1; }

# ── Args ──────────────────────────────────────────────────────────────────────
while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out)        out="${2:?}"; shift 2 ;;
    -d|--dir)        dir="${2:?}"; shift 2 ;;
    -b|--bin)        bin="${2:?}"; shift 2 ;;
    -t|--secs)       secs="${2:?}"; shift 2 ;;
    -m|--media)      media="${2:?}"; shift 2 ;;
    --kill-after)    kill_after="${2:?}"; shift 2 ;;
    --install)       do_install=1; shift ;;
    --uninstall)     do_uninstall=1; shift ;;
    --watchdog)      do_watchdog=1; shift ;;
    --stall)         stall_secs="${2:?}"; shift 2 ;;
    --render-check)  render_check_every="${2:?}"; shift 2 ;;
    -h|--help)       sed -n '2,/^$/{/^# /s/^# \?//p}' "$0"; exit 0 ;;
    *)               die "unknown argument: $1 (try --help)" ;;
  esac
done

# ── systemd user service (auto-resume across session crashes) ─────────────────
uninstall_service() {
  command -v systemctl >/dev/null || die "systemctl not found"
  systemctl --user disable --now "$unit" 2>/dev/null
  rm -f "$unit_path"
  systemctl --user daemon-reload 2>/dev/null
  printf 'removed user service %s\n' "$unit"
  if [ -f "$wd_unit_path" ] && command -v sudo >/dev/null; then
    sudo systemctl disable --now "$wd_unit" 2>/dev/null
    sudo rm -f "$wd_unit_path"
    sudo systemctl daemon-reload 2>/dev/null
    printf 'removed watchdog service %s\n' "$wd_unit"
  fi
}

if [ "$do_uninstall" = 1 ]; then
  uninstall_service
  exit 0
fi

# ── Locate the binary ─────────────────────────────────────────────────────────
if [ -z "$bin" ]; then
  here="$(cd "$(dirname "$0")" && pwd)"
  for cand in \
    "$here/../../build/examples/shadertoy/shadertoy_egl" \
    "$here/../../builddir/examples/shadertoy/shadertoy_egl" \
    "$(command -v shadertoy_egl 2>/dev/null)"; do
    if [ -n "$cand" ] && [ -x "$cand" ]; then bin="$cand"; break; fi
  done
fi
[ -n "$bin" ] && [ -x "$bin" ] || die "shadertoy_egl not found; pass --bin PATH"
[ -d "$dir" ] || die "shader dir not found: $dir"
command -v timeout >/dev/null || die "coreutils 'timeout' is required"
command -v flock >/dev/null || die "util-linux 'flock' is required"

install_service() {
  command -v systemctl >/dev/null || die "systemctl not found (need systemd)"
  mkdir -p "$unit_dir"
  # Absolute output path: a service has no useful CWD, and the watchdog must
  # agree on where the ledger/.cur marker live.
  local out_abs; case "$out" in /*) out_abs="$out" ;; *) out_abs="$PWD/$out" ;; esac
  local exec="$self --out $(printf %q "$out_abs") --dir $(printf %q "$dir")"
  exec="$exec --bin $(printf %q "$bin") --secs $(printf %q "$secs")"
  exec="$exec --kill-after $(printf %q "$kill_after")"
  exec="$exec --render-check $(printf %q "$render_check_every")"
  [ -n "$media" ] && exec="$exec --media $(printf %q "$media")"
  # Carry the binary's shared-library path into the unit so the service-launched
  # shadertoy_egl resolves its libs (e.g. libshadertoy.so); otherwise every
  # shader would fail to load and be wrongly blacklisted.
  local envlines="Environment=GEN_BLACKLIST_SERVICE=1"
  [ -n "${LD_LIBRARY_PATH:-}" ] &&
    envlines="$envlines
Environment=LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
  cat > "$unit_path" <<EOF
[Unit]
Description=Generate shadertoy GPU-hang blacklist (resumes across reboots)
After=default.target
# never give up restarting: each restart still makes forward progress (the
# in-flight shader is recorded and blacklisted on resume)
StartLimitIntervalSec=0

[Service]
Type=simple
$envlines
ExecStart=$exec
# restart on crash/kill (incl. a watchdog reboot), not on clean completion
Restart=on-failure
RestartSec=15

[Install]
WantedBy=default.target
EOF
  # Run headless at boot without an interactive login (any systemd distro).
  loginctl enable-linger "$(id -un)" 2>/dev/null ||
    printf 'note: enable linger for boot-time start: sudo loginctl enable-linger %s\n' \
      "$(id -un)"
  systemctl --user daemon-reload
  systemctl --user enable --now "$unit"
  printf 'installed %s\n' "$unit_path"
  [ "$do_watchdog" = 1 ] && install_watchdog "$out_abs"
  printf 'logs:  journalctl --user -u %s -f\n' "$unit"
  printf 'stop:  %s --uninstall\n' "$self"
}

# Root system service that reboots the machine when the scan hard-hangs the GPU
# (a D-state process timeout/SIGKILL cannot reap). Generic across distros via
# the softdog kernel module — no specific watchdog hardware required.
install_watchdog() {
  local out_abs="$1"
  command -v sudo >/dev/null || die "watchdog needs root: install sudo or run as root"
  # Stall threshold must sit well above a normal per-shader run, or a healthy
  # shader would look hung and trigger a reboot.
  local minstall=$((secs + kill_after + 30))
  [ "$stall_secs" -gt "$minstall" ] ||
    die "--stall ($stall_secs) must exceed --secs+--kill-after+30 (=$minstall)"
  local wd_exec="$self __watchdog $(printf %q "$out_abs.cur") $(printf %q "$stall_secs") $(printf %q "$(id -un)")"
  local tmp; tmp="$(mktemp)"
  cat > "$tmp" <<EOF
[Unit]
Description=shadertoy blacklist watchdog (reboot on a hard GPU hang)
After=default.target

[Service]
Type=simple
ExecStart=$wd_exec
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF
  sudo install -m 0644 "$tmp" "$wd_unit_path" && rm -f "$tmp"
  sudo systemctl daemon-reload
  sudo systemctl enable --now "$wd_unit"
  printf 'installed %s (reboots ~%ss after a hard hang)\n' "$wd_unit_path" "$stall_secs"
}

if [ "$do_install" = 1 ]; then
  install_service
  exit 0
fi

# How each shader is hosted. With an ambient compositor (WAYLAND_DISPLAY set —
# e.g. a desktop session), run the client directly against it. Otherwise — the
# headless/embedded case — host each shader in its own cage compositor, which
# drives DRM/KMS directly and exits when the shader does. A fresh cage per
# shader means a GPU hang takes down only that one run, never a shared session.
launch_prefix=()
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
  : # ambient compositor
elif command -v cage >/dev/null; then
  launch_prefix=(cage --)
else
  die "no compositor: set WAYLAND_DISPLAY, or install cage for headless runs"
fi

# Single-instance lock. A GPU hang can leave an unkillable D-state cage child,
# so a service restart may fail to reap the previous scanner; without this lock
# a second scanner would run alongside the stuck one, each spawning its own cage
# and doubling GPU contention. Hold an exclusive, non-blocking lock for the run;
# a second instance exits immediately (the stuck one's frozen heartbeat still
# trips the watchdog, whose reboot clears the D-state cage and the lock).
exec 200>"$out.lock" 2>/dev/null || die "cannot open lock $out.lock"
flock -n 200 || die "another gen_blacklist scan is already running ($out.lock)"

log="$out.log"   # append-only ledger: "ok <id>" / "bad <id> <reason>"
cur="$out.cur"   # id currently under test (survives a session-killing hang)
beat="$out.beat" # progress heartbeat: mtime bumped every loop iteration so the
                 # watchdog can detect a stall anywhere, even when .cur is empty

# A graceful stop (systemctl stop / Ctrl-C) is not a hang: clear the in-flight
# marker so the watchdog never reads a stale .cur and reboots an idle box. A
# hard hang leaves bash blocked in `timeout` — the trap can't run until that
# returns — so the marker is correctly preserved for blacklist-on-resume; a
# SIGKILL/crash likewise leaves it (no trap runs). Belt-and-suspenders with the
# watchdog's scan_stopped() gate.
trap 'rm -f "$cur" "$beat"; exit 143' TERM INT

# Count GPU resets/hangs so far this boot.  Each DRM driver logs its own marker
# on a hang; match the common ones so detection is not tied to one GPU:
#   amdgpu — "ring <x> timeout" / "GPU reset"
#   v3d (Raspberry Pi) — "<engine> job timed out, resetting"
#   i915 / xe / nouveau / msm — "GPU HANG" / "reset" / "hangcheck" / "fault"
# journalctl -k is readable without sudo for users in the systemd-journal group.
resets() {
  journalctl -k -b 2>/dev/null | grep -ciE \
    'ring [^ ]+ timeout|job timed out|gpu hang|gpu reset|hangcheck|MMU error|device wedged|\*ERROR\* .*reset'
}

# Run shader $1 under the chosen compositor with a timeout; set `rc`.
#
# On a render checkpoint (render sampling on, and N ok shaders since the last
# one) it ALSO captures a liveness frame from this shader's OWN cage run — no
# extra cage start — into `cap_hash`: the run is backgrounded with a slightly
# longer timeout, grim grabs a frame once the shader has had time to render,
# then the run is reaped. The caller compares cap_hash across checkpoints: two
# *different* shaders yielding the same frame means the display is stuck on a
# stale frame (grim reads the compositor buffer, which stops changing once
# cage's present blocks on a wedged KMS pipe). cap_hash is empty on a
# non-checkpoint shader or if the capture fails.
run_shader() {
  local f="$1" cmd
  cap_hash=""
  cmd=("${launch_prefix[@]}" "$bin")
  [ -n "$media" ] && cmd+=(--media "$media")
  cmd+=("$f")

  if [ "$render_on" = 1 ] && [ "$since_check" -ge "$render_check_every" ]; then
    since_check=0
    local rt before after sock grab lim runpid
    rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    grab=$((secs + 1)); lim=$((secs + 3))   # render past startup, then capture
    before="$(ls "$rt"/wayland-* 2>/dev/null | grep -E 'wayland-[0-9]+$' | sort)"
    timeout --kill-after="$kill_after" "$lim" "${cmd[@]}" >/dev/null 2>&1 &
    runpid=$!
    sleep "$grab"
    after="$(ls "$rt"/wayland-* 2>/dev/null | grep -E 'wayland-[0-9]+$' | sort)"
    sock="$(comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after") | head -1)"
    [ -n "$sock" ] &&
      cap_hash="$(WAYLAND_DISPLAY="$sock" grim - 2>/dev/null | cksum 2>/dev/null | cut -d' ' -f1)"
    wait "$runpid"; rc=$?
  else
    [ "$render_on" = 1 ] && since_check=$((since_check + 1))
    timeout --kill-after="$kill_after" "$secs" "${cmd[@]}" >/dev/null 2>&1
    rc=$?
  fi
}

# ── Resume: a leftover marker means the previous run's shader killed us ────────
if [ -s "$cur" ]; then
  victim="$(cat "$cur")"
  printf 'bad %s hang-killed-session\n' "$victim" >> "$log"
  printf 'resuming: "%s" took down the previous run — blacklisted\n' "$victim"
  : > "$cur"
fi
# Clear any render-frozen flag from a prior boot so the watchdog starts clean
# (otherwise it would reboot immediately after the resume → boot loop).
rm -f "$out.render-dead"
: > "$beat"   # fresh heartbeat before the loop so the watchdog trusts it

# Render-liveness sampling is only meaningful when we host shaders ourselves
# (cage) and grim is available; off for an ambient compositor or without grim.
render_on=0
if [ "$render_check_every" -gt 0 ] && [ "${#launch_prefix[@]}" -gt 0 ] &&
   command -v grim >/dev/null; then render_on=1; fi
prev_rhash=""; since_check=0

# Set of ids already decided (resume skip).
declare -A done=()
if [ -f "$log" ]; then
  while read -r verdict id _; do
    [ -n "${id:-}" ] && done["$id"]=1
  done < "$log"
fi

# ── Regenerate the bare-id blacklist from the ledger (last verdict wins) ───────
write_blacklist() {
  declare -A reason=()
  local order=()
  while read -r verdict id rest; do
    [ "$verdict" = bad ] || { unset 'reason[$id]'; continue; }
    [ -n "${reason[$id]:-}" ] || order+=("$id")
    reason["$id"]="${rest:-unknown}"
  done < "$log"
  {
    printf '# shadertoy blacklist — generated by gen_blacklist.sh, do not hand-edit\n'
    printf '# ids that hang the GPU or crash; reasons are in the comments\n'
    printf '# regenerate after editing %s\n\n' "$(basename "$log")"
    local id
    for id in "${order[@]}"; do
      [ -n "${reason[$id]:-}" ] || continue   # was later marked ok
      printf '# %s: %s\n%s\n' "$id" "${reason[$id]}" "$id"
    done
  } > "$out"
}

# ── Collect shaders, deterministic order ──────────────────────────────────────
shopt -s nullglob
files=()
for f in "$dir"/*.json "$dir"/*.frag "$dir"/*.glsl; do files+=("$f"); done
[ "${#files[@]}" -gt 0 ] || die "no .json/.frag/.glsl shaders in $dir"
IFS=$'\n' files=($(printf '%s\n' "${files[@]}" | sort)); unset IFS

printf 'scanning %d shaders in %s (%ss each)\n' "${#files[@]}" "$dir" "$secs"
[ -z "$media" ] || printf 'media dir: %s\n' "$media"

n_ok=0 n_bad=0 n_skip=0
for f in "${files[@]}"; do
  : > "$beat"                          # heartbeat: progress this iteration
  id="$(basename "${f%.*}")"
  if [ -n "${done[$id]:-}" ]; then n_skip=$((n_skip+1)); continue; fi

  printf '%s ... ' "$id"
  printf '%s' "$id" > "$cur"          # in-flight marker (crash-survivable)
  before="$(resets)"

  run_shader "$f"                     # sets rc; on a checkpoint also cap_hash

  sync; after="$(resets)"            # let the journal flush the timeout line
  : > "$cur"

  if [ "$after" -gt "$before" ]; then
    printf 'bad %s gpu-hang\n' "$id" >> "$log"; printf 'GPU HANG\n'; n_bad=$((n_bad+1))
  elif [ "$rc" = 0 ] || [ "$rc" = 124 ] || [ "$rc" = 137 ] || [ "$rc" = 143 ]; then
    printf 'ok %s\n' "$id" >> "$log"; printf 'ok\n'; n_ok=$((n_ok+1))
  else
    printf 'bad %s exit=%s\n' "$id" "$rc" >> "$log"; printf 'crash (exit %s)\n' "$rc"; n_bad=$((n_bad+1))
  fi
  done["$id"]=1
  write_blacklist

  # Render-liveness: on a checkpoint, run_shader captured a frame (cap_hash) from
  # this shader's own cage run (no extra run). Two *different* shaders yielding
  # the same frame means the display is wedged (stuck on a stale frame) while the
  # loop keeps advancing — which progress/heartbeat can't see. Flag the watchdog.
  if [ -n "$cap_hash" ]; then
    if [ "$cap_hash" = "$prev_rhash" ]; then
      printf '\nrender frozen: identical frames across shaders — display wedged\n' >&2
      : > "$cur"                 # do not blame a specific shader
      : > "$out.render-dead"     # signal the watchdog to reboot
      write_blacklist
      if [ -f "$wd_unit_path" ]; then
        printf 'waiting for the watchdog to reboot...\n' >&2
        while :; do sleep 5; done   # hold so a restart can't clear the flag first
      fi
      exit 1
    fi
    prev_rhash="$cap_hash"
  fi
done

rm -f "$cur"
write_blacklist
printf '\ndone: %d ok, %d blacklisted, %d skipped (already tested)\n' \
  "$n_ok" "$n_bad" "$n_skip"
printf 'blacklist: %s   ledger: %s\n' "$out" "$log"
printf 'use it:  %s --cycle 20 --blacklist %s\n' "$(basename "$bin")" "$out"

# Scan finished cleanly: if we were launched by the auto-restart service, retire
# it (and the watchdog) so neither starts again on the next boot. The watchdog
# is harmless once idle (it only reboots on a non-empty, stale .cur), but remove
# it when we can to leave softdog disarmed.
if [ -n "${GEN_BLACKLIST_SERVICE:-}" ]; then
  systemctl --user disable "$unit" 2>/dev/null
  rm -f "$unit_path"
  printf 'scan complete — removed auto-restart service %s\n' "$unit"
  if [ -f "$wd_unit_path" ]; then
    sudo -n systemctl disable --now "$wd_unit" 2>/dev/null &&
      sudo -n rm -f "$wd_unit_path" 2>/dev/null &&
      printf 'removed watchdog service %s\n' "$wd_unit" ||
      printf 'note: remove the watchdog with: %s --uninstall\n' "$self"
  fi
fi
