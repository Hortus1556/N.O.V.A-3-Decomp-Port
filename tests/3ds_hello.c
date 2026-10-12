// New 3DS packaging check (tools/3ds_cia.py, tools/3ds_run.py): a .cxi/.cia with the port's
// exheader (host/3ds/nova3.rsf) that reports what the native port relies on, on the bottom
// screen and in sdmc:/3ds/nova3/hello.log:
//  * the memory mode: the application region's size and what is free (178 MB mode: ~178 MB);
//  * a regular heap larger than libctru's default, as runtime/os_3ds.cpp allocates it (the game's
//    image plus the newlib heap), and the linear heap with the rest;
//  * svcControlProcessMemory: a heap page with ARM code made read + execute, then called;
//  * the CPU mode (804 MHz, L2) and whether this is an emulator (Citra/Azahar's GetSystemInfo).
#include <3ds.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
void mappableInit(u32 addr_min, u32 addr_max);

// as runtime/os_3ds.cpp: the regular heap as large as the heap region allows (up to the main stack), the linear
// heap (GPU memory) gets what is left
void __system_allocateHeaps(void) {
    Handle limit = 0;
    s64 max = 0, used = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE);
    svcGetResourceLimitLimitValues(&max, limit, &type, 1);
    svcGetResourceLimitCurrentValues(&used, limit, &type, 1);
    svcCloseHandle(limit);
    const u32 remaining = (u32)(max - used) & ~0xfffu;
    // the heap region ends at the main thread's stack (mapped just below 0x10000000)
    MemInfo mi;
    PageInfo pi;
    const u32 stack = R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u32)&limit)) ? mi.base_addr : 0x0e000000u;
    const u32 heap_max = (stack < 0x10000000u ? stack : 0x10000000u) - OS_HEAP_AREA_BEGIN - 0x1000;
    u32 heap = remaining > (24u << 20) ? remaining - (24u << 20) : remaining / 2;
    if (heap > heap_max) heap = heap_max;
    heap &= ~0xfffu;
    u32 tmp = 0;
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    __ctru_heap_size = heap;
    if (R_FAILED(svcControlMemory(&tmp, __ctru_heap, 0, heap, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE)))
        svcBreak(USERBREAK_PANIC);
    __ctru_linear_heap_size = remaining - heap;
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                                  MEMPERM_READ | MEMPERM_WRITE)))
        svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

static FILE *g_log;

static void say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/nova3", 0777);
    g_log = fopen("sdmc:/3ds/nova3/hello.log", "w");

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    s64 emu = 0;
    const bool emulator = R_SUCCEEDED(svcGetSystemInfo(&emu, 0x20000, 0));
    say("new3ds %d, emulator %d", n3ds, emulator);
    say("application region %lu KB, free %lu KB", (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10),
        (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
    say("heap %lu KB at %08lx, linear %lu KB at %08lx", (unsigned long)(__ctru_heap_size >> 10), (unsigned long)__ctru_heap,
        (unsigned long)(__ctru_linear_heap_size >> 10), (unsigned long)__ctru_linear_heap);
    // the largest single block newlib can hand out (the game's heap lives there)
    u32 big = 0;
    for (u32 mb = 160; mb >= 8; mb -= 8) {
        void *p = malloc((size_t)mb << 20);
        if (p) { big = mb; free(p); break; }
    }
    say("largest malloc %lu MB", (unsigned long)big);

    // a page of code: mov r0, #42; bx lr
    u32 *code = (u32 *)memalign(0x1000, 0x1000);
    code[0] = 0xe3a0002a;
    code[1] = 0xe12fff1e;
    Handle self = 0;
    Result rd = svcDuplicateHandle(&self, CUR_PROCESS_HANDLE);
    Result rf = svcFlushProcessDataCache(self, (u32)code, 0x1000);
    Result rp = svcControlProcessMemory(self, (u32)code, 0, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_EXECUTE);
    say("duplicate %08lx, flush %08lx, ControlProcessMemory(PROT, RX) %08lx", (unsigned long)rd, (unsigned long)rf,
        (unsigned long)rp);
    MemInfo mi;
    PageInfo pi;
    svcQueryMemory(&mi, &pi, (u32)code);
    say("page %08lx: perm %lx state %lx", (unsigned long)mi.base_addr, (unsigned long)mi.perm, (unsigned long)mi.state);
    int r = ((int (*)(void))code)();
    say("code page returned %d", r);
    say("HELLO DONE");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    if (g_log) fclose(g_log);
    gfxExit();
    return 0;
}
