#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Dual-build sync gate (docs/cmake_plan.md §8). Asserts the Meson and CMake
# builds stay in lock-step on the surface that is duplicated between them:
#
#   1. the installed framework header set AND contents,
#   2. the installed wayland-cxx.pc,
#   3. both install the scanner executable (tool-install parity),
#   4. each ime_backend maps to the SAME WL_IME_BACKEND_* compile define.
#
# Exits non-zero on any divergence. Runnable locally and from CI.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PREFIX=/usr

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== Installing Meson build → $WORK/mdest =="
meson setup "$WORK/mb" "$ROOT" --prefix="$PREFIX" -Dtests=false -Dexamples=false >/dev/null
DESTDIR="$WORK/mdest" meson install -C "$WORK/mb" >/dev/null

echo "== Installing CMake build → $WORK/cdest =="
cmake -S "$ROOT" -B "$WORK/cb" -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$WORK/cb" >/dev/null
DESTDIR="$WORK/cdest" cmake --install "$WORK/cb" >/dev/null

M="$WORK/mdest$PREFIX"
C="$WORK/cdest$PREFIX"

# ── 1. Header set ────────────────────────────────────────────────────────────
( cd "$M" && find include -type f | sort ) > "$WORK/m.list"
( cd "$C" && find include -type f | sort ) > "$WORK/c.list"
if ! diff -u "$WORK/m.list" "$WORK/c.list"; then
    fail "installed header set differs (Meson vs CMake)"
fi
echo "OK: header set identical ($(wc -l < "$WORK/m.list") files)"

# ── 1b. Header contents ──────────────────────────────────────────────────────
while read -r h; do
    diff -q "$M/$h" "$C/$h" >/dev/null || fail "header content differs: $h"
done < "$WORK/m.list"
echo "OK: all header contents identical"

# ── 2. pkg-config (.pc) — same prefix → must be byte-identical ───────────────
MPC="$(find "$M" -name 'wayland-cxx.pc' | head -1)"
CPC="$(find "$C" -name 'wayland-cxx.pc' | head -1)"
[ -n "$MPC" ] || fail "Meson did not install wayland-cxx.pc"
[ -n "$CPC" ] || fail "CMake did not install wayland-cxx.pc"
if ! diff -u "$MPC" "$CPC"; then
    fail "wayland-cxx.pc differs"
fi
echo "OK: wayland-cxx.pc identical"

# ── 3. Scanner exe install parity (both default to installing it) ────────────
test -e "$M/bin/wayland-cxx-scanner" || fail "Meson did not install the scanner exe"
test -e "$C/bin/wayland-cxx-scanner" || fail "CMake did not install the scanner exe"
echo "OK: scanner exe installed by both"

# ── 4. IME define parity across every backend choice ─────────────────────────
for bk in none text-input-v1 text-input-v3 input-method-v1 input-method-v2 virtual-keyboard-v1; do
    cdef="$(cmake -S "$ROOT" -B "$WORK/imec" -G Ninja -DWAYLAND_CXX_IME_BACKEND="$bk" 2>/dev/null \
        | sed -n 's/.*(-D\(WL_IME_BACKEND_[A-Z0-9_]*\)).*/\1/p' | head -1)"
    rm -rf "$WORK/imec"
    meson setup "$WORK/imem" "$ROOT" -Dime_backend="$bk" -Dtests=false -Dexamples=false >/dev/null 2>&1
    mdef="$(grep -ohE 'WL_IME_BACKEND_[A-Z0-9_]+' "$WORK/imem/compile_commands.json" | head -1)"
    rm -rf "$WORK/imem"
    [ -n "$cdef" ] || fail "could not extract CMake IME define for $bk"
    [ -n "$mdef" ] || fail "could not extract Meson IME define for $bk"
    [ "$cdef" = "$mdef" ] || fail "ime_backend=$bk → Meson=$mdef CMake=$cdef"
    echo "OK: ime_backend=$bk → $cdef (both)"
done

echo "ALL DUAL-BUILD SYNC CHECKS PASSED"
