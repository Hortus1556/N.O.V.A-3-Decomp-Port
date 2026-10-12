cmake_minimum_required(VERSION 3.20)
project(nova3 C CXX)

# Compilers: clang-cl on Windows, clang on Linux/macOS (the generated code relies on clang
# builtins such as __builtin_setjmp).
set(CMAKE_CXX_STANDARD 20)
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Directory with recompiled guest code (recomp/arm2c.py gen)")

if(NINTENDO_3DS)
    # devkitARM (GCC) through devkitPro's toolchain file (/opt/devkitpro/cmake/3DS.cmake: -march=armv6k
    # -mtune=mpcore -mfloat-abi=hard). No generated code: the game's own ARM code runs natively.
    # Guest pointers are host pointers (g_mem = 0): no null-pointer reasoning about g_mem + address.
    add_compile_options(-fno-strict-aliasing -fwrapv -ffunction-sections -fno-delete-null-pointer-checks)
elseif(NINTENDO_SWITCH)
    # devkitA64 (GCC) through devkitPro's toolchain file: aarch64-none-elf-cmake
    add_compile_options(-fno-strict-aliasing -fwrapv -ffunction-sections)
    set(GEN_FLAGS -ffp-contract=off -w)
elseif(NINTENDO_GAMECUBE)
    # devkitPPC (GCC) with libogc2 through tools/gc/gamecube.cmake (docs/gamecube.md). Guest
    # addresses are host addresses (recomp.h: HOST() takes the address itself).
    add_compile_options(-fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections -fno-delete-null-pointer-checks)
    # the translated code: no small-data area (its constants go to .rodata, which is paged with
    # the code: tools/gc/ld/ogc.ld) and no unwind tables (3.4 MB of MEM1; nothing unwinds through
    # it: the guest's C++ exceptions are runtime/eh.cpp's)
    set(GEN_FLAGS -ffp-contract=off -w -G 0 -fno-asynchronous-unwind-tables -fno-unwind-tables)
elseif(MSVC)
    # Line tables + PDB so crashes can be mapped back to guest functions/instructions.
    add_compile_options(/Z7 /clang:-gline-tables-only)
    add_link_options(/DEBUG)
    set(GEN_FLAGS /clang:-ffp-contract=off)
else()
    add_compile_options(-gline-tables-only -fno-strict-aliasing -fwrapv)
    set(GEN_FLAGS -ffp-contract=off)
endif()
set(GEN_WARNINGS -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-value
    -Wno-parentheses-equality)

if(NOT NINTENDO_3DS)
    file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
endif()
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${GEN_FLAGS};${GEN_WARNINGS}")
# native replacements of hot guest functions: the same float rules (no contraction), so their
# results match the recompiled originals bit for bit
set_source_files_properties(host/native_hot.cpp PROPERTIES COMPILE_OPTIONS "${GEN_FLAGS}")

# Guest code and runtime reference each other (dispatch table <-> rt_dispatch), so they
# form one library. The OS layer is the only platform-specific part.
set(RUNTIME_SOURCES runtime/runtime.cpp runtime/thread.cpp runtime/libc_string.cpp
    runtime/libc_io.cpp runtime/libc_misc.cpp runtime/eh.cpp runtime/pgo.cpp
    runtime/jni.cpp runtime/java.cpp runtime/gles_null.cpp runtime/gles.cpp runtime/net.cpp)
if(NINTENDO_3DS)
    # the native code loader (runtime/native_3ds.*) instead of the recompiled code's dispatch, and
    # dlmalloc (extern/dlmalloc) as the guest heap, apart from newlib's (no runtime/heap.cpp)
    enable_language(ASM)
    list(APPEND RUNTIME_SOURCES runtime/os_3ds.cpp runtime/native_3ds.cpp runtime/native_3ds_gates.S
        runtime/guest_heap_3ds.c)
    # GL: runtime/gles.cpp over the PICA200 backend (runtime/gles_pica.cpp, citro3d) and its vertex
    # shaders (runtime/pica/*.v.pica: one DVLB, embedded as pica_shbin)
    set(PICA_SHADERS runtime/pica/generic.v.pica runtime/pica/lit.v.pica runtime/pica/point2.v.pica)
    set(PICA_SHBIN "${CMAKE_BINARY_DIR}/pica.shbin")
    add_custom_command(OUTPUT "${PICA_SHBIN}"
        COMMAND "${CTR_PICASSO_EXE}" -o "${PICA_SHBIN}" ${PICA_SHADERS}
        DEPENDS ${PICA_SHADERS} WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" VERBATIM)
    find_program(BIN2S_EXE NAMES bin2s HINTS "${DEVKITPRO}/tools/bin" REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/pica_shbin.s" "${CMAKE_BINARY_DIR}/pica_shbin.h"
        COMMAND "${BIN2S_EXE}" -a 4 -H pica_shbin.h pica.shbin > pica_shbin.s
        DEPENDS "${PICA_SHBIN}" WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    list(APPEND RUNTIME_SOURCES runtime/gles_pica.cpp runtime/gl_program.cpp "${CMAKE_BINARY_DIR}/pica_shbin.s")
    # zlib (devkitPro's 3ds-zlib): the library's compressed copy (runtime/native_3ds.cpp)
    find_package(ZLIB REQUIRED)
    set(OS_LIBS ZLIB::ZLIB)
elseif(NINTENDO_SWITCH)
    list(APPEND RUNTIME_SOURCES runtime/heap.cpp runtime/os_switch.cpp)
    set(OS_LIBS)
elseif(NINTENDO_GAMECUBE)
    # the pager (MMU page faults to ARAM and the disc), the OS layer on libogc2, log output to the
    # IPL UART; no sockets (net_none.cpp); GL on GX (gles_gx.cpp)
    enable_language(ASM)
    list(REMOVE_ITEM RUNTIME_SOURCES runtime/net.cpp)
    list(APPEND RUNTIME_SOURCES runtime/gles_gx.cpp runtime/gl_program.cpp runtime/heap.cpp runtime/os_gc.cpp runtime/net_none.cpp runtime/gc_pager.c
        runtime/gc_pager_stub.S runtime/gc_report.c runtime/gc_dvdfs.c runtime/gc_ramfs.cpp runtime/gc_atomic.c
        runtime/gc_mprof.c)
    set(OS_LIBS)
elseif(WIN32)
    list(APPEND RUNTIME_SOURCES runtime/heap.cpp runtime/os_win.cpp)
    set(OS_LIBS dbghelp ws2_32 iphlpapi winmm)
else()
    list(APPEND RUNTIME_SOURCES runtime/heap.cpp runtime/os_posix.cpp)
    find_package(Threads REQUIRED)
    set(OS_LIBS Threads::Threads ${CMAKE_DL_LIBS})
endif()
add_library(nova3_core STATIC ${GEN_SOURCES} ${RUNTIME_SOURCES})
target_include_directories(nova3_core PUBLIC runtime "${GEN_DIR}")
target_link_libraries(nova3_core PUBLIC ${OS_LIBS})
if(NINTENDO_SWITCH)
    # It is linked into the PIE executable: -fPIE, not the -fPIC devkitPro's platform file gives a
    # static library. With -fPIC GCC treats every global function as interposable: no inlining
    # or interprocedural optimisation of the translated functions, globals read through the GOT.
    set_target_properties(nova3_core PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(nova3_core PRIVATE -fPIE)
endif()
if(NINTENDO_3DS)
    # the hook table, trampolines and kernel helper fixups: python tools/v6patch.py, then
    # python tools/hooktable.py (they need the game library and devkitARM; see docs/3ds.md)
    set(NATIVE_TABLE "${CMAKE_SOURCE_DIR}/build/3ds/native_table.inc")
    if(NOT EXISTS "${NATIVE_TABLE}")
        message(FATAL_ERROR "missing ${NATIVE_TABLE}: run python tools/v6patch.py, then python tools/hooktable.py")
    endif()
    target_include_directories(nova3_core PRIVATE "${CMAKE_SOURCE_DIR}/build/3ds")
    set_source_files_properties(runtime/native_3ds.cpp PROPERTIES OBJECT_DEPENDS "${NATIVE_TABLE}")
endif()

# Profile-guided optimisation of the generated code and the runtime (nova3_core); the workflow
# is in docs/recompiler.md. NOVA3_PGO=generate builds an instrumented game whose runs write
# profiles (runtime/pgo.cpp writes them when the process ends); NOVA3_PGO=use optimises with
# them. NOVA3_PGO_DIR: where an instrumented PC build writes its .profraw files, and where a
# use build finds the merged profile (PC: nova3.profdata; Switch: the .gcda files).
set(NOVA3_PGO "" CACHE STRING "profile-guided optimisation: generate, use, or empty (off)")
set(NOVA3_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "profiles: written (PC generate) or read (use)")
set(NOVA3_PGO_OUT "sdmc:/switch/nova3/pgo" CACHE STRING "Switch generate: where the console writes the .gcda files")
if(NOVA3_PGO)
    if(NINTENDO_SWITCH)
        # GCC names a profile after the object path relative to the build directory, as GCC sees
        # that directory (a Windows path under MSYS2): generate and use builds must share it.
        set(PGO_ROOT "${CMAKE_BINARY_DIR}")
        if(CMAKE_HOST_SYSTEM_NAME MATCHES "MSYS|MINGW|CYGWIN|Windows")
            if(PGO_ROOT MATCHES "^/([a-zA-Z])/(.*)$")
                string(TOUPPER "${CMAKE_MATCH_1}" PGO_DRIVE)
                set(PGO_ROOT "${PGO_DRIVE}:/${CMAKE_MATCH_2}")
            endif()
            string(REPLACE "/" "\\" PGO_ROOT "${PGO_ROOT}")
        endif()
        if(NOVA3_PGO STREQUAL "generate")
            set(PGO_FLAGS -fprofile-generate=pgo "-fprofile-prefix-path=${PGO_ROOT}" -fprofile-info-section=nova3_gcov)
            target_compile_definitions(nova3_core PRIVATE NOVA3_PGO_GENERATE "NOVA3_PGO_OUT=\"${NOVA3_PGO_OUT}\"")
            target_link_options(nova3_core INTERFACE -fprofile-generate)
        else()
            set(PGO_FLAGS "-fprofile-use=${NOVA3_PGO_DIR}" "-fprofile-prefix-path=${PGO_ROOT}" -fprofile-partial-training
                -Wno-missing-profile)
        endif()
    else()
        if(NOVA3_PGO STREQUAL "generate")
            set(PGO_FLAGS "-fprofile-instr-generate=${NOVA3_PGO_DIR}/nova3-%p.profraw")
            target_compile_definitions(nova3_core PRIVATE NOVA3_PGO_GENERATE)
            if(MSVC)
                # link.exe / lld-link do not add the profile runtime by themselves
                execute_process(COMMAND "${CMAKE_C_COMPILER}" /clang:-print-resource-dir
                                OUTPUT_VARIABLE CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
                target_link_libraries(nova3_core PUBLIC "${CLANG_RESOURCE_DIR}/lib/windows/clang_rt.profile-x86_64.lib")
            else()
                target_link_options(nova3_core INTERFACE -fprofile-instr-generate)
            endif()
        else()
            set(PGO_FLAGS "-fprofile-instr-use=${NOVA3_PGO_DIR}/nova3.profdata" -Wno-profile-instr-unprofiled
                -Wno-profile-instr-out-of-date)
        endif()
        if(MSVC)
            list(TRANSFORM PGO_FLAGS PREPEND "/clang:")
        endif()
    endif()
    target_compile_options(nova3_core PRIVATE ${PGO_FLAGS})
    message(STATUS "PGO ${NOVA3_PGO}: ${PGO_FLAGS}")
endif()

if(NINTENDO_3DS)
    # The New 3DS port: host/3ds/main.cpp with the shared game-side host modules; libctru. The
    # .cxi / .cia come from tools/3ds_cia.py (the exheader: host/3ds/nova3.rsf). See docs/3ds.md.
    add_executable(nova3 host/3ds/main.cpp host/3ds/dspfirm.cpp host/input.cpp host/hooks.cpp host/settings.cpp host/audio.cpp
        host/eventlog.cpp host/offline.cpp host/prompts.cpp host/controls_menu.cpp)
    # host/sdl.h: only SDL3's scancode list (no SDL library on the 3DS)
    target_include_directories(nova3 PRIVATE host "${CMAKE_SOURCE_DIR}/extern/sdl/include")
    target_link_libraries(nova3 PRIVATE nova3_core citro3d)
    target_link_options(nova3 PRIVATE "LINKER:--whole-archive" $<TARGET_FILE:nova3_core> "LINKER:--no-whole-archive")
    # newlib's heap checked at every free and allocation (runtime/guest_heap_3ds.c)
    target_link_options(nova3 PRIVATE "LINKER:--wrap=_malloc_r,--wrap=_free_r,--wrap=_realloc_r,--wrap=_memalign_r")
    # the packaging check: memory mode, heap layout, executable pages (tools/3ds_cia.py, 3ds_run.py)
    add_executable(nova3_hello EXCLUDE_FROM_ALL tests/3ds_hello.c)
    return()
endif()

if(NINTENDO_GAMECUBE)
    # The GameCube port: host/gc/main.cpp with the shared game-side host modules; libogc2. The link
    # takes tools/gc/ld/ogc.ld (-B: devkitPPC's -mogc spec finds it first), which puts the translated
    # code at 0x40000000; tools/gc_dol.py splits the ELF into the DOL and the paged code file.
    add_executable(nova3 host/gc/main.cpp host/input.cpp host/hooks.cpp host/settings.cpp host/audio.cpp
        host/eventlog.cpp host/offline.cpp host/prompts.cpp host/controls_menu.cpp host/native_hot.cpp)
    target_include_directories(nova3 PRIVATE host "${CMAKE_SOURCE_DIR}/extern/sdl/include")
    target_link_libraries(nova3 PRIVATE nova3_core fat ogc m)  # (fat: libdvm, tools/gc/build_libdvm.sh)
    # the paged code's order (tools/gc_order.py from a PGO profile), INCLUDEd by ogc.ld: empty when
    # there is none
    set(NOVA3_GC_ORDER "${CMAKE_SOURCE_DIR}/build/gc/gcode_order.ld" CACHE FILEPATH "function order of the paged code")
    if(EXISTS "${NOVA3_GC_ORDER}")
        configure_file("${NOVA3_GC_ORDER}" "${CMAKE_BINARY_DIR}/gcode_order.ld" COPYONLY)
    else()
        file(WRITE "${CMAKE_BINARY_DIR}/gcode_order.ld" "/* no profile */\n")
    endif()
    target_link_options(nova3 PRIVATE "-L${CMAKE_BINARY_DIR}" "-B${CMAKE_SOURCE_DIR}/tools/gc/ld/" "LINKER:--whole-archive" $<TARGET_FILE:nova3_core>
        "LINKER:--no-whole-archive" "LINKER:-Map=${CMAKE_BINARY_DIR}/nova3.map")
    # the host heap by allocation site (runtime/gc_mprof.c): bring-up only
    option(NOVA3_GC_MPROF "GameCube: profile the host heap by allocation site" OFF)
    if(NOVA3_GC_MPROF)
        set_source_files_properties(runtime/gc_mprof.c TARGET_DIRECTORY nova3_core PROPERTIES COMPILE_DEFINITIONS NOVA3_GC_MPROF)
        target_link_options(nova3 PRIVATE "LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=free")
    endif()
    set_target_properties(nova3 PROPERTIES SUFFIX ".elf")
    return()
endif()

if(NINTENDO_SWITCH)
    # The Switch port (homebrew .nro / .nsp): host/switch/main.cpp with the shared game-side
    # host modules, SDL2 + GLES 2 (Mesa) from devkitPro's portlibs. See docs/switch.md.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NX_SDL2 REQUIRED IMPORTED_TARGET sdl2 egl glesv2)
    add_executable(nova3 host/switch/main.cpp host/input.cpp host/hooks.cpp host/settings.cpp host/audio.cpp
        host/eventlog.cpp host/offline.cpp host/prompts.cpp host/controls_menu.cpp host/native_hot.cpp)
    target_include_directories(nova3 PRIVATE host)
    target_link_libraries(nova3 PRIVATE nova3_core PkgConfig::NX_SDL2)
    # runtime/os_switch.cpp logs host throws and (NOVA3_ALLOC_LOG) large heap blocks
    target_link_options(nova3 PRIVATE "LINKER:--wrap=__cxa_throw,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=aligned_alloc,--wrap=posix_memalign,--wrap=free")
    target_link_options(nova3 PRIVATE "LINKER:--whole-archive" $<TARGET_FILE:nova3_core> "LINKER:--no-whole-archive")
    nx_generate_nacp(nova3.nacp NAME "N.O.V.A. 3" AUTHOR "port" VERSION "0.1.0")
    nx_create_nro(nova3 NACP nova3.nacp)
    # The exefs for an installable title (tools/switch_nsp.py packs it with hacBrewPack).
    nx_create_exefs(nova3_exefs TARGET nova3 CONFIG "${CMAKE_SOURCE_DIR}/host/switch/nova3.json")
    return()
endif()

# SDL3: the release package in extern/sdl on Windows, the system package elsewhere.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/extern/sdl/cmake/SDL3Config.cmake")
    find_package(SDL3 REQUIRED CONFIG PATHS "${CMAKE_SOURCE_DIR}/extern/sdl/cmake" NO_DEFAULT_PATH)
else()
    find_package(SDL3 REQUIRED CONFIG)
endif()

# Dear ImGui (extern/imgui, MIT) for the launcher window.
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/extern/imgui")
add_library(imgui STATIC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_executable(nova3 host/main.cpp host/input.cpp host/hooks.cpp host/settings.cpp host/launcher.cpp host/audio.cpp host/eventlog.cpp host/offline.cpp host/prompts.cpp host/controls_menu.cpp host/discord.cpp host/texpack.cpp host/native_hot.cpp)
target_link_libraries(nova3 PRIVATE nova3_core SDL3::SDL3 imgui)
if(WIN32)
    # A GUI program (no console window); the log goes to nova3.log or an optional console.
    set_target_properties(nova3 PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(nova3 PRIVATE /ENTRY:mainCRTStartup)
    target_sources(nova3 PRIVATE host/nova3.manifest)  # UTF-8 process code page (file paths)
    # The game's icon (from the APK, tools/make_icon.py) as resource 1: Explorer, the taskbar,
    # and the windows (SDL_HINT_WINDOWS_INTRESOURCE_ICON in host/main.cpp).
    find_program(PYTHON_EXE NAMES python python3)
    set(NOVA3_APK "${CMAKE_SOURCE_DIR}/NOVA-3-v1-0-1d.apk")
    if(PYTHON_EXE AND EXISTS "${NOVA3_APK}")
        set(ICON_DIR "${CMAKE_BINARY_DIR}/icon")
        add_custom_command(OUTPUT "${ICON_DIR}/nova3.ico"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ICON_DIR}"
            COMMAND ${PYTHON_EXE} "${CMAKE_SOURCE_DIR}/tools/make_icon.py" "${NOVA3_APK}" "${ICON_DIR}/nova3.ico"
            DEPENDS "${NOVA3_APK}" "${CMAKE_SOURCE_DIR}/tools/make_icon.py")
        file(WRITE "${ICON_DIR}/nova3.rc" "1 ICON \"nova3.ico\"\n")
        set_source_files_properties("${ICON_DIR}/nova3.rc" PROPERTIES OBJECT_DEPENDS "${ICON_DIR}/nova3.ico")
        target_sources(nova3 PRIVATE "${ICON_DIR}/nova3.rc")
    endif()
endif()
# Imports register themselves from static initializers: keep every runtime object.
if(MSVC)
    target_link_options(nova3 PRIVATE /WHOLEARCHIVE:nova3_core.lib)
    add_custom_command(TARGET nova3 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:nova3>)
    # the packaged folder (tools/package.py), when there is one, gets the new nova3.exe
    # (experimental build folders turn it off: -DNOVA3_COPY_TO_DIST=OFF)
    option(NOVA3_COPY_TO_DIST "copy nova3.exe into dist/ after building" ON)
    if(NOVA3_COPY_TO_DIST)
        add_custom_command(TARGET nova3 POST_BUILD COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:nova3>
                           -DDIST=${CMAKE_SOURCE_DIR}/dist -P ${CMAKE_SOURCE_DIR}/tools/copy_to_dist.cmake)
    endif()
else()
    target_link_options(nova3 PRIVATE "LINKER:--whole-archive" $<TARGET_FILE:nova3_core> "LINKER:--no-whole-archive"
                        -rdynamic)  # -rdynamic: crash reports name f_XXXXXXXX frames via dladdr
endif()

# Differential instruction tester (tests/difftest.py generates the snippets).
set(DIFF_GEN_DIR "${CMAKE_SOURCE_DIR}/build/diff/gen")
if(EXISTS "${DIFF_GEN_DIR}/snippets.c")
    add_executable(difftest EXCLUDE_FROM_ALL tests/difftest.cpp ${RUNTIME_SOURCES}
        "${DIFF_GEN_DIR}/snippets.c")
    target_include_directories(difftest PRIVATE runtime)
    target_link_libraries(difftest PRIVATE ${OS_LIBS})
    set_source_files_properties("${DIFF_GEN_DIR}/snippets.c" PROPERTIES COMPILE_OPTIONS "${GEN_FLAGS};${GEN_WARNINGS}")
endif()
