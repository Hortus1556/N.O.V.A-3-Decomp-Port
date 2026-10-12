// Pager test (runtime/gc_pager.c) in Dolphin or on a console: python tools/gc_run.py build/gc/pager_test.dol
// 1. 8 MB of anonymous memory over 2 MB of frames: every page written, then read back
//    (evictions to ARAM and reloads), twice;
// 2. a code page filled from a source and run (instruction faults);
// 3. discarded pages read as zeros again.
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "../../runtime/gc_pager.h"
#include "../../runtime/gc_report.h"

#define MB (1024u * 1024u)

static void code_source(void *ctx, uint32_t offset, void *page) {
    (void)ctx;
    uint32_t *w = (uint32_t *)page;
    for (uint32_t i = 0; i < 1024; i += 2) {
        w[i] = 0x38630000u | ((offset >> 12) + 42u);  // addi r3,r3,(page + 42)
        w[i + 1] = 0x4e800020u;                        // blr
    }
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("gc: check failed: " __VA_ARGS__); printf("\n"); } } while (0)

static void report(const char *when, u64 t0) {
    PagerStats s;
    pager_stats(&s);
    printf("gc: %s: %u ms; faults data %u code %u; fills zero %u aram %u source %u; evictions %u, writebacks %u; "
           "aram %u/%u sectors, %u pages; %u us in faults (%u us each)\n",
           when, (unsigned)ticks_to_millisecs(gettime() - t0), s.faults_data, s.faults_code, s.fills_zero, s.fills_aram,
           s.fills_source, s.evictions, s.writebacks, s.aram_used, s.aram_sectors, s.aram_pages, (unsigned)ticks_to_microsecs(s.fault_ticks),
           (unsigned)(ticks_to_microsecs(s.fault_ticks) / (s.faults_data + s.faults_code ? s.faults_data + s.faults_code : 1)));
}

int main(void) {
    gc_report_install();
    VIDEO_Init();
    const u32 aram_base = AR_Init(NULL, 0);
    const u32 aram_size = AR_GetSize() - aram_base;
    void *frames = memalign(4096, 2 * MB);
    printf("gc: pager test: frames %p (2 MB), ARAM %08x + %u KB\n", frames, aram_base, aram_size >> 10);
    pager_init(frames, 2 * MB, aram_base, aram_size);

    // 1. anonymous memory larger than the frames
    const u32 base = 0x20000000u, size = 8 * MB;
    pager_map_zero(base, size);
    u64 t0 = gettime();
    volatile u32 *m = (volatile u32 *)base;
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 a = 0; a < size / 4; a += 256) m[a] = a * 2654435761u + pass;
        u32 bad = 0;
        for (u32 a = 0; a < size / 4; a += 256)
            if (m[a] != a * 2654435761u + pass) bad++;
        CHECK(bad == 0, "pass %u: %u words wrong", pass, bad);
        CHECK(m[1] == 0, "a word never written is %08x", m[1]);
    }
    report("anonymous 8 MB over 2 MB, two passes", t0);

    // 1b. whole pages through the compression: random (stored as they are), repetitive, mixed,
    // and pages written back to zeros (dropped); every word checked
    t0 = gettime();
    const u32 base2 = 0x21000000u, size2 = 6 * MB;
    pager_map_zero(base2, size2);
    volatile u32 *c = (volatile u32 *)base2;
    for (int pass = 0; pass < 2; pass++) {
        u32 x = 12345u + (u32)pass;
        for (u32 i = 0; i < size2 / 4; i++) {
            const u32 page = i >> 10, kind = page % 4;
            x = x * 1664525u + 1013904223u;
            c[i] = kind == 0 ? x : kind == 1 ? (i & 7) * 0x01010101u : kind == 2 ? ((i & 64) ? x >> 24 : i >> 3) : 0;
        }
        u32 bad = 0;
        x = 12345u + (u32)pass;
        for (u32 i = 0; i < size2 / 4; i++) {
            const u32 page = i >> 10, kind = page % 4;
            x = x * 1664525u + 1013904223u;
            if (c[i] != (kind == 0 ? x : kind == 1 ? (i & 7) * 0x01010101u : kind == 2 ? ((i & 64) ? x >> 24 : i >> 3) : 0)) bad++;
        }
        CHECK(bad == 0, "compressed pages, pass %d: %u words wrong", pass, bad);
    }
    report("6 MB of mixed pages over 2 MB, two passes", t0);

    // 2. code from a source
    t0 = gettime();
    const u32 code = 0x40000000u;
    pager_map_source(code, 64 * 1024, 1, code_source, NULL);
    for (u32 page = 0; page < 16; page++) {
        int (*fn)(int) = (int (*)(int))(code + page * 4096 + 8 * page);
        int r = fn(1000);
        CHECK(r == 1000 + 42 + (int)page, "code page %u returned %d", page, r);
    }
    report("code pages", t0);

    // 3. discard
    m[0] = 123;
    pager_discard(base, 4096);
    CHECK(m[0] == 0, "a discarded page reads %u", m[0]);

    printf(fails ? "gc: FAIL (%d)\n" : "gc: done\n", fails);
    for (;;) VIDEO_WaitVSync();
}
