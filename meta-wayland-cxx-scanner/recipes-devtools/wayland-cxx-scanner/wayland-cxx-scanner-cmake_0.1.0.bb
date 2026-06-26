SUMMARY = "Wayland protocol scanner generating C++17 CRTP headers (CMake build)"
HOMEPAGE = "https://github.com/jwinarske/wayland-cxx-scanner"
BUGTRACKER = "https://github.com/jwinarske/wayland-cxx-scanner/issues"
SECTION = "devel"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=9a9e9e58a7e67bbeafadf0f75d86a15c"

SRC_URI = "git://github.com/jwinarske/wayland-cxx-scanner.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"
PV = "0.1.0+git${SRCPV}"
S = "${WORKDIR}/git"

inherit cmake pkgconfig features_check

DEPENDS = "pugixml"

# Policy knobs (weak defaults — overridable without clobbering EXTRA_OECMAKE).
#
# WAYLAND_CXX_IME_BACKEND is a COMPOSITOR capability, not a project default:
# GNOME/Mutter use text-input-v3; Weston/AGL stacks use text-input-v1 /
# input-method-v1. Its authoritative home is the compositor/BSP layer conf
# (set there with ?=, which beats this ??=). Until those layers declare it,
# local.conf is the only override point:
#   WAYLAND_CXX_IME_BACKEND = "text-input-v3"
#   WAYLAND_CXX_IME_BACKEND:pn-wayland-cxx-scanner-cmake = "virtual-keyboard-v1"
# Valid: none text-input-v1 text-input-v3 input-method-v1 input-method-v2
#        virtual-keyboard-v1 (CMake validates; immaterial to the TOOL_ONLY
#        native pass). WAYLAND_CXX_WERROR = "OFF" relaxes -Werror.
WAYLAND_CXX_IME_BACKEND ??= "text-input-v1"
WAYLAND_CXX_WERROR      ??= "ON"

EXTRA_OECMAKE = " \
    -DWAYLAND_CXX_WERROR=${WAYLAND_CXX_WERROR} \
    -DWAYLAND_CXX_IME_BACKEND=${WAYLAND_CXX_IME_BACKEND} \
"

PACKAGECONFIG ??= ""

# Ship a target-arch scanner on the device (built from source for the target).
PACKAGECONFIG[tool] = "-DWAYLAND_CXX_SCANNER_BUILD_TOOL=ON,-DWAYLAND_CXX_SCANNER_BUILD_TOOL=OFF,"

PACKAGECONFIG[examples] = "-DWAYLAND_CXX_SCANNER_BUILD_EXAMPLES=ON,-DWAYLAND_CXX_SCANNER_BUILD_EXAMPLES=OFF,\
    wayland-cxx-scanner-cmake-native \
    wayland wayland-protocols \
    libxkbcommon \
    virtual/egl \
    virtual/libgles2 \
"

PACKAGECONFIG[tests] = "-DWAYLAND_CXX_SCANNER_BUILD_TESTS=ON,-DWAYLAND_CXX_SCANNER_BUILD_TESTS=OFF,googletest"

REQUIRED_DISTRO_FEATURES = "${@bb.utils.contains('PACKAGECONFIG', 'examples', 'wayland opengl', '', d)}"

BBCLASSEXTEND = "native nativesdk"

# Native/nativesdk only need pugixml (the scanner does not link wayland). In a
# native build CMAKE_CROSSCOMPILING is false, so the exe is always built and
# INSTALL_TOOL (default ON) installs it — no PACKAGECONFIG override needed.
DEPENDS:class-native    = "pugixml-native"
DEPENDS:class-nativesdk = "pugixml-nativesdk"

FILES:${PN}-dev += " \
    ${includedir}/wl/ \
    ${libdir}/pkgconfig/wayland-cxx.pc \
    ${libdir}/cmake/wayland-cxx-scanner/ \
"

ALLOW_EMPTY:${PN} = "1"
