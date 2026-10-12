#!/bin/sh
# Builds and installs libdvm's FAT library (extremscorner's libfat for libogc2: FatFs behind
# newlib devices, fatMountSimple) for the GameCube port's SD card (docs/gamecube.md, Building)
# into /opt/devkitpro/libogc2/gamecube, beside libogc2 (tools/gc/build_libogc2.sh first). Run
# from the devkitPro MSYS2 shell (C:\msys64\usr\bin\bash.exe -l). Pinned to a known commit.
set -e
COMMIT=420b9ae7beff18c361cbbfbe7671cabdf3dd26a5
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-/tmp/libdvm-build}"
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH
[ -d "$WORK/.git" ] || git clone https://github.com/extremscorner/libdvm.git "$WORK"
cd "$WORK"
git fetch -q origin "$COMMIT" || true
git checkout -q -f "$COMMIT"
git submodule update --init -q
L=$DEVKITPRO/libogc2/gamecube
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/gamecube.cmake" -DCMAKE_BUILD_TYPE=Release \
      -DOGC_ROOT="$L" -DOGC_SUBDIR=. -DCMAKE_INSTALL_PREFIX="$L" -DCMAKE_INSTALL_LIBDIR=lib
ninja -C build -j4 fat dvm
cp -f build/libfat.a build/libdvm.a "$L/lib/"
cp -f include/fat.h include/dvm.h "$L/include/"
echo "$COMMIT" > "$L/LIBDVM_COMMIT"
