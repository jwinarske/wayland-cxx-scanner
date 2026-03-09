# SPDX-License-Identifier: MIT
# BitBake recipe for wayland-cxx-scanner
#
# Provides:
#   wayland-cxx-scanner      — the protocol scanner executable
#   wayland-cxx-scanner-dev  — C++23 framework headers + pkg-config file
#   wayland-cxx-scanner-native/nativesdk — scanner tool for use in builds

SUMMARY = "Wayland protocol scanner that generates C++23 CRTP client/server headers"
DESCRIPTION = "\
wayland-cxx-scanner parses Wayland XML protocol files and generates \
WTL-patterned C++23 client and server headers using CRTP, along with a \
lightweight framework library (wl::CProxy, wl::CRegistry, etc.)."

HOMEPAGE = "https://github.com/jwinarske/wayland-cxx-scanner"
BUGTRACKER = "https://github.com/jwinarske/wayland-cxx-scanner/issues"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=9a9e9e58a7e67bbeafadf0f75d86a15c"

SRC_URI = "git://github.com/jwinarske/wayland-cxx-scanner.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"
PV = "0.1.0+git${SRCPV}"

S = "${WORKDIR}/git"

inherit meson pkgconfig

# pugixml is in meta-oe; wayland is in core (oe-core).
DEPENDS = "pugixml wayland"

# Disable tests and examples for target/native builds — they require a
# running compositor and are not needed in a production image.
EXTRA_OEMESON = " \
    -Dtests=false \
    -Dexamples=false \
"

# Framework headers land under ${includedir}/wl/
FILES:${PN}-dev += " \
    ${includedir}/wl/ \
    ${libdir}/pkgconfig/wayland-cxx.pc \
"

# The scanner tool is architecture-independent in usage but arch-specific
# in build; expose it as a native/nativesdk tool so it can be invoked during
# other packages' do_compile.
BBCLASSEXTEND = "native nativesdk"

# Native builds only need pugixml (no Wayland runtime headers required
# because the scanner does not link against wayland-client/server).
DEPENDS:class-native = "pugixml-native"
DEPENDS:class-nativesdk = "pugixml-nativesdk"

EXTRA_OEMESON:class-native = " \
    -Dtests=false \
    -Dexamples=false \
"
EXTRA_OEMESON:class-nativesdk = " \
    -Dtests=false \
    -Dexamples=false \
"
