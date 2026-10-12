// The GameCube's host heap by allocation site (docs/gamecube.md, Memory profiles): a bring-up
// tool behind the CMake option NOVA3_GC_MPROF, which links malloc, calloc, realloc, memalign
// and free (libogc2's dlmalloc) through the wrappers here. Each block gets a 16-byte header
// naming its site (three return addresses from malloc's caller up); gc_mprof_dump() lists the
// sites holding the most bytes, for powerpc-eabi-addr2line.
#include "gc_mprof.h"

#ifndef NOVA3_GC_MPROF
void gc_mprof_dump(const char *when, unsigned top) { (void)when, (void)top; }
#else

#include <ogc/irq.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_realloc(void *p, size_t n);
void *__real_memalign(size_t align, size_t n);

#define MAGIC 0xA11Cu
#define SITES 2048

struct header {  // just before the block
    uint16_t magic, site;
    uint32_t offset;  // from the real block's start to the caller's
    uint32_t size;    // asked for
    uint32_t pad;
};
struct site {
    uint32_t ra[3];
    uint32_t live, blocks, calls;
};
static struct site sites[SITES];
static uint32_t total_live;

static uint16_t site_of(uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t h = (a * 2654435761u) ^ (b * 40503u) ^ (c * 97u);
    for (uint32_t i = 0; i < SITES; i++) {
        const uint16_t k = (uint16_t)((h + i) % SITES);
        struct site *s = &sites[k];
        if (s->ra[0] == a && s->ra[1] == b && s->ra[2] == c) return k;
        if (!s->calls) {
            s->ra[0] = a, s->ra[1] = b, s->ra[2] = c;
            return k;
        }
    }
    return 0;  // (full: counted with slot 0's)
}

static void *tag(void *real, uint32_t offset, size_t n, uint32_t a, uint32_t b, uint32_t c) {
    if (!real) return 0;
    struct header *h = (struct header *)((char *)real + offset) - 1;
    const uint32_t level = IRQ_Disable();
    const uint16_t s = site_of(a, b, c);
    sites[s].live += (uint32_t)n, sites[s].blocks++, sites[s].calls++;
    total_live += (uint32_t)n;
    IRQ_Restore(level);
    h->magic = MAGIC, h->site = s, h->offset = offset, h->size = (uint32_t)n;
    return h + 1;
}
static struct header *header_of(void *p) {
    struct header *h = (struct header *)p - 1;
    return h->magic == MAGIC ? h : 0;
}
static void *untag(void *p) {
    struct header *h = header_of(p);
    if (!h) return p;  // (not ours)
    const uint32_t level = IRQ_Disable();
    sites[h->site].live -= h->size, sites[h->site].blocks--;
    total_live -= h->size;
    IRQ_Restore(level);
    h->magic = 0;
    return (char *)(h + 1) - h->offset;
}

#define RA(n) ((uint32_t)(uintptr_t)__builtin_return_address(n))
#define HDR sizeof(struct header)

void *__wrap_malloc(size_t n) { return tag(__real_malloc(n + HDR), HDR, n, RA(0), RA(1), RA(2)); }
void *__wrap_calloc(size_t m, size_t n) {
    void *p = tag(__real_malloc(m * n + HDR), HDR, m * n, RA(0), RA(1), RA(2));
    if (p) memset(p, 0, m * n);
    return p;
}
void __wrap_free(void *p) {
    if (p) __real_free(untag(p));
}
void *__wrap_memalign(size_t align, size_t n) {
    if (align < HDR) align = HDR;
    return tag(__real_memalign(align, n + align), (uint32_t)align, n, RA(0), RA(1), RA(2));
}
void *__wrap_realloc(void *p, size_t n) {
    struct header *h = p ? header_of(p) : 0;
    if (p && !h) return __real_realloc(p, n);
    void *q = tag(__real_malloc(n + HDR), HDR, n, RA(0), RA(1), RA(2));
    if (!q || !p) return q;
    memcpy(q, p, h->size < n ? h->size : n);
    __real_free(untag(p));
    return q;
}

void gc_mprof_dump(const char *when, unsigned top) {
    static struct site copy[SITES];
    const uint32_t level = IRQ_Disable();
    memcpy(copy, sites, sizeof copy);
    const uint32_t total = total_live;
    IRQ_Restore(level);
    fprintf(stderr, "mprof (%s): %u KB live; by site (KB, blocks, calls: return addresses)\n", when, (unsigned)(total >> 10));
    for (unsigned k = 0; k < top; k++) {
        int best = -1;
        for (int i = 0; i < SITES; i++)
            if (copy[i].live && (best < 0 || copy[i].live > copy[best].live)) best = i;
        if (best < 0) break;
        const struct site *s = &copy[best];
        fprintf(stderr, "mprof %6u %6u %7u: %08x %08x %08x\n", (unsigned)(s->live >> 10), (unsigned)s->blocks,
                (unsigned)s->calls, (unsigned)s->ra[0], (unsigned)s->ra[1], (unsigned)s->ra[2]);
        copy[best].live = 0;
    }
}
#endif
