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
#   4. each ime_backend maps to the SAME WL_IME_BACKEND_* compile define,
#   5. every protocol header both builds generate is generated with the SAME
#      scanner flags.
#
# Check 5 exists because the first four all passed while the CMake build was
# generating agl_shell_client.hpp without --emit-interface-tables and failing to
# link: nothing here compared how the scanner is actually invoked, only what
# ends up installed, and generated headers are not installed.
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

# ── 5. Scanner invocation parity ─────────────────────────────────────────────
# Both build systems invoke the scanner once per generated protocol header, and
# the flags decide what the header contains.  Compare the invocations rather
# than the outputs: the generated headers are not installed, so checks 1-3 never
# see them, and a missing flag surfaces only as a link error in whichever build
# forgot it.
#
# The key is (call site, protocol XML, output header) and the value is the flag
# set.  The call site is the last directory of the output path — the example or
# test name, which both builds agree on even though they lay out their build
# trees differently.  It has to be part of the key: two examples may legitimately
# generate the same header from the same XML with different flags, because one
# hand-writes its interface tables and the other has the scanner emit them.
# Examples and tests are both enabled so every call site is covered.
echo "== Comparing scanner invocations =="
# Keep the configure logs: when this fails it is almost always a missing
# dependency, and swallowing the reason turns a one-line fix into a CI round
# trip.
if ! meson setup "$WORK/gm" "$ROOT" -Dtests=true -Dexamples=true \
        >"$WORK/gm.log" 2>&1; then
    tail -20 "$WORK/gm.log" >&2
    fail "meson configure (tests+examples) failed — see above"
fi
if ! cmake -S "$ROOT" -B "$WORK/gc" -G Ninja \
        -DWAYLAND_CXX_SCANNER_BUILD_EXAMPLES=ON \
        -DWAYLAND_CXX_SCANNER_BUILD_TESTS=ON >"$WORK/gc.log" 2>&1; then
    tail -20 "$WORK/gc.log" >&2
    fail "cmake configure (tests+examples) failed — see above"
fi

# Pull "--mode=X [--flags] <path>.xml <output>" out of the generated ninja files.
# Anchored on --mode= because the source tree path itself contains the string
# "wayland-cxx-scanner" and would otherwise match.
scanner_invocations() {
    grep -ohE -- "--mode=[a-z-]+( --[a-z-]+)* [^ ]+\.xml [^ &|]+" "$1/build.ninja" \
    | awk '{
        xml = ""; out = ""; flags = "";
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^--/)         { flags = flags " " $i }
            else if ($i ~ /\.xml$/) { n = split($i, a, "/"); xml = a[n] }
            else {
                # Keep the last two path components: "<call site>/<header>".
                n = split($i, a, "/");
                out = (n > 1 ? a[n-1] "/" a[n] : a[n]);
            }
        }
        print out " from " xml ":" flags
    }' | sort -u
}

scanner_invocations "$WORK/gm" > "$WORK/inv.meson"
scanner_invocations "$WORK/gc" > "$WORK/inv.cmake"
[ -s "$WORK/inv.meson" ] || fail "no scanner invocations found in the Meson build"
[ -s "$WORK/inv.cmake" ] || fail "no scanner invocations found in the CMake build"

# Compare flags only where both builds generate the same header from the same
# XML.  A header only one build generates is reported, not failed: the two
# builds legitimately cover different sets of examples and tests today.
cut -d: -f1 "$WORK/inv.meson" | sort -u > "$WORK/keys.meson"
cut -d: -f1 "$WORK/inv.cmake" | sort -u > "$WORK/keys.cmake"
comm -12 "$WORK/keys.meson" "$WORK/keys.cmake" > "$WORK/keys.shared"

divergent=0
while read -r key; do
    # No quotes around the greps' failure: a key comes from these same files, so
    # a miss means the extraction is broken, not that the flags differ — but
    # `set -e` would kill the script silently rather than say so.
    mflags="$(grep -F "$key:" "$WORK/inv.meson" | sed 's/^[^:]*://' | sort -u)" \
        || fail "key vanished from the Meson invocation list: $key"
    cflags="$(grep -F "$key:" "$WORK/inv.cmake" | sed 's/^[^:]*://' | sort -u)" \
        || fail "key vanished from the CMake invocation list: $key"
    if [ "$mflags" != "$cflags" ]; then
        echo "  DIVERGED: $key" >&2
        echo "      Meson:$mflags" >&2
        echo "      CMake:$cflags" >&2
        divergent=1
    fi
done < "$WORK/keys.shared"
[ "$divergent" -eq 0 ] || fail "scanner flags differ between Meson and CMake"
echo "OK: scanner flags identical for all $(wc -l < "$WORK/keys.shared") shared protocol headers"

# Informational: not a failure, but the asymmetry should be visible rather than
# silently absorbed.
comm -23 "$WORK/keys.meson" "$WORK/keys.cmake" | sed 's/^/  only Meson generates: /'
comm -13 "$WORK/keys.meson" "$WORK/keys.cmake" | sed 's/^/  only CMake generates: /'

echo "ALL DUAL-BUILD SYNC CHECKS PASSED"
