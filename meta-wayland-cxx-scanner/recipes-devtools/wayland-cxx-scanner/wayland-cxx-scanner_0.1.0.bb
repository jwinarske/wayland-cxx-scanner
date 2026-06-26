# SPDX-License-Identifier: MIT
# BitBake recipe for wayland-cxx-scanner (Meson build — canonical).
#
# Provides:
#   wayland-cxx-scanner      — the protocol scanner executable (PACKAGECONFIG tool)
#   wayland-cxx-scanner-dev  — C++17 framework headers + pkg-config file
#   wayland-cxx-scanner-native/nativesdk — scanner tool for use in builds
#
# Kept structurally parallel with the CMake variant recipe
# (wayland-cxx-scanner-cmake_0.1.0.bb); the option names differ (Meson vs CMake)
# but the policy knobs and PACKAGECONFIG resolve the same questions.

SUMMARY = "Wayland protocol scanner generating C++17 CRTP client/server headers"
DESCRIPTION = "\
wayland-cxx-scanner parses Wayland XML protocol files and generates \
WTL-patterned C++17 client and server headers using CRTP, along with a \
header-only framework (wl::CProxy, wl::CRegistry, etc.)."

HOMEPAGE = "https://github.com/jwinarske/wayland-cxx-scanner"
BUGTRACKER = "https://github.com/jwinarske/wayland-cxx-scanner/issues"
SECTION = "devel"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=9a9e9e58a7e67bbeafadf0f75d86a15c"

SRC_URI = "git://github.com/jwinarske/wayland-cxx-scanner.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"
PV = "0.1.0+git${SRCPV}"
S = "${WORKDIR}/git"

inherit meson pkgconfig features_check

# The scanner links only pugixml; wayland/xkbcommon/EGL are example-only deps,
# pulled in by PACKAGECONFIG[examples].
DEPENDS = "pugixml"

# Policy knobs (weak defaults — overridable without clobbering EXTRA_OEMESON).
#
# WAYLAND_CXX_IME_BACKEND is a COMPOSITOR capability, not a project default:
# GNOME/Mutter use text-input-v3; Weston/AGL stacks use text-input-v1 /
# input-method-v1. Its authoritative home is the compositor/BSP layer conf
# (set there with ?=, which beats this ??=). Until those layers declare it,
# local.conf is the only override point:
#   WAYLAND_CXX_IME_BACKEND = "text-input-v3"
#   WAYLAND_CXX_IME_BACKEND:pn-wayland-cxx-scanner = "virtual-keyboard-v1"
# Valid: none text-input-v1 text-input-v3 input-method-v1 input-method-v2
#        virtual-keyboard-v1 (Meson validates the combo). WAYLAND_CXX_WERROR =
#        "OFF" relaxes -Werror.
WAYLAND_CXX_IME_BACKEND ??= "text-input-v1"
WAYLAND_CXX_WERROR      ??= "ON"

EXTRA_OEMESON = " \
    -Dwerror=${@'true' if d.getVar('WAYLAND_CXX_WERROR') == 'ON' else 'false'} \
    -Dime_backend=${WAYLAND_CXX_IME_BACKEND} \
"

PACKAGECONFIG ??= ""

# Ship the scanner binary in ${PN} on the target. Meson always builds the exe;
# install_tool gates whether it is installed — the same "is there a scanner in
# ${PN}?" question the CMake recipe answers with BUILD_TOOL.
PACKAGECONFIG[tool] = "-Dinstall_tool=true,-Dinstall_tool=false,"

PACKAGECONFIG[examples] = "-Dexamples=true,-Dexamples=false,\
    wayland wayland-protocols \
    libxkbcommon \
    virtual/egl \
    virtual/libgles2 \
"

PACKAGECONFIG[tests] = "-Dtests=true,-Dtests=false,googletest"

REQUIRED_DISTRO_FEATURES = "${@bb.utils.contains('PACKAGECONFIG', 'examples', 'wayland opengl', '', d)}"

BBCLASSEXTEND = "native nativesdk"

# Native/nativesdk only need pugixml, and MUST install the tool so it can be
# invoked during other packages' do_compile (target/${PN} ships it only when
# the 'tool' PACKAGECONFIG is enabled).
DEPENDS:class-native        = "pugixml-native"
DEPENDS:class-nativesdk     = "pugixml-nativesdk"
PACKAGECONFIG:class-native    = "tool"
PACKAGECONFIG:class-nativesdk = "tool"

# Framework headers land under ${includedir}/wl/
FILES:${PN}-dev += " \
    ${includedir}/wl/ \
    ${libdir}/pkgconfig/wayland-cxx.pc \
"

ALLOW_EMPTY:${PN} = "1"
