#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Build Skia and install it into a prefix consumed by the Skia examples via
# meson's -Dskia_prefix=<prefix>.  This script is the single source of truth
# for the pinned revision and the canonical GN args; CI and developers both
# run it so their builds match.
#
# The installed prefix is self-contained and laid out so Skia's own headers
# resolve unchanged (they reference each other as "include/core/..." and
# "modules/<m>/include/..."):
#
#   <prefix>/include/...      core public headers
#   <prefix>/modules/...      module public headers (skparagraph, skshaper, ...)
#   <prefix>/lib/*.a          static libraries
#
# Consumers add `-isystem <prefix>` and `-L<prefix>/lib`.
#
# Usage:
#   scripts/build-skia.sh --prefix <install-dir> [--src <checkout-dir>]
#                         [--jobs N] [--config release|debug]

set -euo pipefail

# ── Pinned revision ───────────────────────────────────────────────────────────
# Skia has no conventional stable tags; the stable-quality branches are the
# chrome/mNNN milestone branches that track Chrome's release train.  m150 is the
# current stable milestone (Chrome 150, stable 2026-06-30).  Bump deliberately:
# the CI cache key is (SKIA_REF x compiler x arch x sanitizer).
SKIA_REPO="https://skia.googlesource.com/skia.git"
SKIA_REF="chrome/m150"

# ── Defaults ──────────────────────────────────────────────────────────────────
PREFIX=""
SRC_DIR=""
JOBS="$(nproc 2>/dev/null || echo 4)"
CONFIG="release"

die() {
    echo "build-skia: $*" >&2
    exit 1
}

usage() {
    sed -n '3,24p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="${2:-}"; shift 2 ;;
        --src)    SRC_DIR="${2:-}"; shift 2 ;;
        --jobs)   JOBS="${2:-}"; shift 2 ;;
        --config) CONFIG="${2:-}"; shift 2 ;;
        -h|--help) usage 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done

[ -n "$PREFIX" ] || die "--prefix is required (see --help)"
[ -n "$SRC_DIR" ] || SRC_DIR="$(pwd)/skia-src"

case "$CONFIG" in
    release) IS_DEBUG="false" ;;
    debug)   IS_DEBUG="true" ;;
    *) die "--config must be 'release' or 'debug'" ;;
esac

command -v git >/dev/null 2>&1 || die "git not found"
command -v python3 >/dev/null 2>&1 || die "python3 not found"

# ── Checkout at the pinned ref ────────────────────────────────────────────────
if [ ! -d "$SRC_DIR/.git" ]; then
    echo "build-skia: cloning $SKIA_REPO ($SKIA_REF) into $SRC_DIR"
    git clone "$SKIA_REPO" "$SRC_DIR"
fi

git -C "$SRC_DIR" fetch --tags origin
git -C "$SRC_DIR" checkout "$SKIA_REF"

echo "build-skia: syncing third-party dependencies"
python3 "$SRC_DIR/tools/git-sync-deps"

# ── Fetch the pinned gn/ninja that ship with Skia ─────────────────────────────
GN="$SRC_DIR/bin/gn"
if [ ! -x "$GN" ]; then
    python3 "$SRC_DIR/bin/fetch-gn"
fi
NINJA="$SRC_DIR/third_party/ninja/ninja"
if [ ! -x "$NINJA" ]; then
    python3 "$SRC_DIR/bin/fetch-ninja"
fi

# ── Configure ─────────────────────────────────────────────────────────────────
# Raster + GL + Vulkan + EGL, with the SkParagraph text stack.  One build feeds
# every Skia example; the raster example links only what it references.  Bundled
# HarfBuzz/ICU keep text shaping reproducible across hosts.
OUT_DIR="$SRC_DIR/out/${CONFIG}"
GN_ARGS="\
is_official_build=true is_component_build=false is_debug=${IS_DEBUG} \
skia_use_system_harfbuzz=false skia_use_system_icu=false \
skia_use_system_libpng=false skia_use_system_libjpeg_turbo=false \
skia_use_system_libwebp=false skia_use_system_zlib=false \
skia_use_system_expat=false \
skia_use_gl=true skia_use_vulkan=true skia_use_egl=true \
skia_use_icu=true skia_use_harfbuzz=true \
skia_enable_skparagraph=true \
skia_enable_svg=false skia_enable_pdf=false skia_enable_skottie=false \
extra_cflags=[\"-fPIC\"]"

echo "build-skia: gn gen $OUT_DIR"
# --root pins the source tree: gn otherwise looks for the .gn marker relative to
# the current directory, which is the caller's cwd, not the Skia checkout.
"$GN" gen "$OUT_DIR" --root="$SRC_DIR" --args="$GN_ARGS"

# ── Build ─────────────────────────────────────────────────────────────────────
echo "build-skia: ninja (-j$JOBS)"
"$NINJA" -C "$OUT_DIR" -j "$JOBS" \
    skia skparagraph skshaper skunicode_core skunicode_icu

# ── Install ───────────────────────────────────────────────────────────────────
echo "build-skia: installing into $PREFIX"
mkdir -p "$PREFIX/lib"

# Header trees, preserving the layout Skia's own #includes expect.
cp -a "$SRC_DIR/include" "$PREFIX/"

# Public module headers (SkUnicode, SkParagraph, ...) reference private headers
# under src/ as "src/base/SkUTF.h" and similar, so ship the src/ header tree
# (headers only) rooted at the prefix.
if command -v rsync >/dev/null 2>&1; then
    rsync -am --include='*/' --include='*.h' --exclude='*' \
        "$SRC_DIR/src/" "$PREFIX/src/"
else
    ( cd "$SRC_DIR" && find src -name '*.h' -print0 \
        | cpio -0 -pdm --quiet "$PREFIX" )
fi

mkdir -p "$PREFIX/modules"
for m in skparagraph skshaper skunicode; do
    if [ -d "$SRC_DIR/modules/$m/include" ]; then
        mkdir -p "$PREFIX/modules/$m"
        cp -a "$SRC_DIR/modules/$m/include" "$PREFIX/modules/$m/"
    fi
done

# skcms has no include/ subdir: the public core header is referenced as
# "modules/skcms/skcms.h" and it in turn pulls "src/skcms_public.h".
if [ -f "$SRC_DIR/modules/skcms/skcms.h" ]; then
    mkdir -p "$PREFIX/modules/skcms/src"
    cp -a "$SRC_DIR/modules/skcms/skcms.h" "$PREFIX/modules/skcms/"
    cp -a "$SRC_DIR/modules/skcms/src/"*.h "$PREFIX/modules/skcms/src/"
fi

# Static libraries.
found_any=false
for lib in "$OUT_DIR"/*.a; do
    [ -e "$lib" ] || continue
    cp -a "$lib" "$PREFIX/lib/"
    found_any=true
done
[ "$found_any" = true ] || die "no static libraries produced in $OUT_DIR"

cat <<EOF
build-skia: done.

  Revision : $SKIA_REF
  Prefix   : $PREFIX

Configure the examples with:

  meson setup build -Dexamples=true -Dskia_examples=enabled -Dskia_prefix=$PREFIX
EOF
