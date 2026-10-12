// The GameCube's host heap by allocation site (runtime/gc_mprof.c; CMake option NOVA3_GC_MPROF).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// The `top` sites holding the most bytes, to stderr. Without NOVA3_GC_MPROF: nothing.
void gc_mprof_dump(const char *when, unsigned top);
#ifdef __cplusplus
}
#endif
