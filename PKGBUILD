# Maintainer: Ark Royal <awright42mk1@protonmail.com>
# Based on kwin-x11 by Felix Yan and Antonio Rojas

pkgname=sonic-win
pkgver=6.6.4.1.r92.g779e791b53
pkgrel=1
pkgdesc='An easy to use, but flexible, X Window Manager (X11 fork with Vulkan support)'
arch=(x86_64)
url='https://github.com/Sonic-DE/sonic-win'
license=(LGPL-2.0-or-later)
depends=(aurorae
         breeze
         gcc-libs
         glibc
         plasma-activities
         kauth
         kcmutils
         kcolorscheme
         kconfig
         kcoreaddons
         kcrash
         kdeclarative
         kdecoration
         kglobalaccel
         kglobalacceld
         kguiaddons
         ki18n
         kirigami
         kitemmodels
         knewstuff
         knighttime
         knotifications
         kpackage
         kquickcharts
         kscreenlocker
         kservice
         ksvg
         kwidgetsaddons
         kwindowsystem
         kxmlgui
         lcms2
         libcanberra
         libdisplay-info
         libdrm
         libepoxy
         libqaccessibilityclient-qt6
         libx11
         libxcb
         libxi
         libxkbcommon
         libxkbcommon-x11
         mesa
         libplasma
         qt6-5compat
         qt6-base
         qt6-declarative
         qt6-sensors
         qt6-svg
         qt6-tools
         xcb-util-cursor
         xcb-util-keysyms
         xcb-util-wm)
makedepends=(extra-cmake-modules
             kdoctools
             python
             vulkan-headers)
optdepends=('vulkan-driver: Vulkan compositing backend')
options=(!strip)
provides=(kwin-x11)
conflicts=(kwin-x11)
_srcdir=/home/user/devel/claude/sonic-win
source=()
sha256sums=()

pkgver() {
  cd "$_srcdir"
  git describe --tags | sed 's/\([^-]*-g\)/r\1/;s/-/./g'
}

build() {
  cmake -B "$_srcdir/src/build" -S "$_srcdir" \
    -DCMAKE_INSTALL_LIBEXECDIR=lib \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF
  cmake --build "$_srcdir/src/build" -- -j 16
}

package() {
  DESTDIR="$pkgdir" cmake --install "$_srcdir/src/build"
}
