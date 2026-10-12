# CMake toolchain of the GameCube port (docs/gamecube.md, "Building"): devkitPPC with libogc2
# as tools/gc/build_libogc2.sh installs it (not devkitPro's libogc). From devkitPro's MSYS2 shell:
#   cmake -S . -B build/gc/cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/gc/gamecube.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DGEN_DIR=build/gen_gc
include($ENV{DEVKITPRO}/cmake/devkitPPC.cmake)

set(NINTENDO_GAMECUBE TRUE)
set(OGC2_ROOT $ENV{DEVKITPRO}/libogc2/gamecube)
set(GC_MACHDEP "-DGEKKO -D__GAMECUBE__ -mogc -mcpu=750 -meabi -mhard-float")
set(CMAKE_C_FLAGS_INIT "${GC_MACHDEP}")
set(CMAKE_CXX_FLAGS_INIT "${GC_MACHDEP}")
set(CMAKE_ASM_FLAGS_INIT "${GC_MACHDEP}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${GC_MACHDEP} -L${OGC2_ROOT}/lib")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${OGC2_ROOT}/include)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${OGC2_ROOT}/include)
set(CMAKE_ASM_STANDARD_INCLUDE_DIRECTORIES ${OGC2_ROOT}/include)
