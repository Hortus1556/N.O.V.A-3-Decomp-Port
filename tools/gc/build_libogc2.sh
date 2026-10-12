#!/bin/sh
# Builds and installs libogc2 for the GameCube port (docs/gamecube.md, Building) into
# /opt/devkitpro/libogc2/gamecube, beside devkitPro's libogc. Run from the devkitPro MSYS2 shell
# (C:\msys64\usr\bin\bash.exe -l). Needs devkitPPC, gamecube-tools and ppc-libmad (devkitPro).
#
# libogc2 is pinned to a known commit, with local changes (libogc2.patch):
#  * EXI_Sync polls the channel's own control register instead of its alias 0x80 bytes higher,
#    which libogc2 reads since "Fix EXI_Sync on Wii U" (Nov 2025). Dolphin 2503 does not map the
#    alias, so every program hung in __exi_init; a GameCube or a Wii in GameCube mode answers on both.
#  * More LWP objects (lwp_config.h): 48 threads (16), 2048 mutexes (64), 1024 condition variables
#    (64), 256 semaphores (64). Every std::mutex and newlib lock is an LWP mutex, and the game has
#    many threads and thousands of locks; the pools come from the 1 MB kernel workspace.
set -e
COMMIT=02988cfa0c03196b94375cb7245f98d9d397f195
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-/tmp/libogc2-build}"
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
[ -d "$WORK/.git" ] || git clone https://github.com/extremscorner/libogc2.git "$WORK"
cd "$WORK"
git fetch -q origin "$COMMIT" || true
git checkout -q -f "$COMMIT"
git apply "$HERE/libogc2.patch"
make -j8 cube
L=$DEVKITPRO/libogc2
mkdir -p $L/gamecube/lib
cp -fr include $L/gamecube/
cp -f lib/cube/*.a $L/gamecube/lib/
cp -f *_license.txt *_rules $L/
echo "$COMMIT + libogc2.patch" > $L/gamecube/COMMIT
