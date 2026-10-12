#!/bin/sh
# Builds the GameCube test programs (tests/gc/*.c) against libogc2 into build/gc/:
#   sh tests/gc/build.sh pager_test
# Run them with tools/gc_run.py.
set -e
cd "$(dirname "$0")/../.."
DKP=/c/msys64/opt/devkitpro
OGC=$DKP/libogc2/gamecube
export PATH="$DKP/devkitPPC/bin:$DKP/tools/bin:$PATH"
CFLAGS="-DGEKKO -mogc -mcpu=750 -meabi -mhard-float -O2 -Wall -I$OGC/include"
mkdir -p build/gc
for t in "$@"; do
    extra="runtime/gc_report.c"
    [ "$t" = pager_test ] && extra="$extra runtime/gc_pager.c runtime/gc_pager_stub.S"
    powerpc-eabi-gcc $CFLAGS tests/gc/$t.c $extra -L$OGC/lib -logc -lm -o build/gc/$t.elf
    elf2dol build/gc/$t.elf build/gc/$t.dol
done
