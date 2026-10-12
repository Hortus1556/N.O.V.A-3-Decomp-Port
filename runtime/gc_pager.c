/* GameCube demand pager: see gc_pager.h and docs/gamecube.md ("Memory tiers").
 *
 * Gekko MMU facts used here (IBM Gekko User's Manual, chapter 7): the page table is a hash
 * table of 8-entry groups (PTEGs) at SDR1; a page's primary group is indexed by
 * (VSID ^ page index) and its secondary by the complement. Each PTE has the virtual segment
 * id, the abbreviated page index, the physical page, referenced (R) and changed (C) bits that
 * the hardware sets, WIMG and the protection (PP). The TLB caches entries: after changing a
 * PTE the old translation must be invalidated (tlbie). Supervisor accesses with segment key
 * Ks = 0 may read and write PP 0-2 pages and only read PP 3 pages.
 *
 * Built without floating point use: the handler runs in exception context, where the
 * interrupted thread's FPRs are live (check: no lfd / stfd in the object).
 */
#include "gc_pager.h"

#include <gccore.h>
#include <ogc/aram.h>
#include <ogc/cache.h>
#include <ogc/context.h>
#include <ogc/machine/processor.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void __exception_sethandler(u32 nExcept, void (*pHndl)(frame_context *));
void pager_dsi_stub(frame_context *);
void pager_isi_stub(frame_context *);

#define PAGE 4096u
#define HTAB_BYTES (128u * 1024u) /* 2048 groups, 16384 entries: few overflows with ~6000 frames */
/* Paged segments (one VSID each): 0x00000000-0x7fffffff, and 0xf0000000-0xffffffff for the
 * guest's top pages (the ARM vector page); libogc's BATs cover 0x80000000-0xdfffffff. */
#define PAGED_SEGMENT(seg) ((seg) < 8u || (seg) == 15u)
#define CHUNK_PAGES 256u          /* page states are kept per 1 MB chunk, allocated on use */
#define NO_FRAME 0xffffffffu
#define NO_SLOT 0xffffu
/* ARAM holds pages compressed, in 256-byte sectors (DMA wants 32-byte multiples): 1 to 16 a
 * page (16: stored as it is), found by first fit in a bitmap from a roving start. */
#define SECTOR 256u
#define SECTORS_MAX 65535u

/* Page state (one word per virtual page). */
enum {
    K_UNMAPPED = 0,
    K_ZERO = 1,   /* anonymous: zeros, then whatever was written */
    K_SOURCE = 2, /* read-only, from a source range */
};
#define ST_KIND(s) ((s) >> 30)
#define ST_EXEC 0x20000000u     /* the CPU may run it */
#define ST_RESIDENT 0x10000000u /* low bits: the frame */
#define ST_ARAM 0x08000000u     /* not resident: low bits its first ARAM sector, ST_NSEC its sectors */
#define ST_LOADING 0x04000000u  /* a fault is filling it from its source (interrupts on) */
#define ST_SRC(s) (((s) >> 16) & 0x7ffu) /* K_SOURCE: the range (when not resident or in ARAM) */
#define ST_LOW(s) ((s) & 0xffffu)
#define ST_NSEC(s) ((((s) >> 16) & 15u) + 1u) /* K_ZERO in ARAM: its sectors */

typedef struct Frame {
    uint32_t vpage;     /* virtual page number, NO_FRAME: free */
    uint16_t slot;      /* first ARAM sector still holding this page's contents, or NO_SLOT */
    uint16_t pte;       /* index of its PTE in the table */
    uint8_t nsec;       /* (slot's sectors) */
    uint8_t busy;       /* being filled from a source: not to be evicted */
} Frame;

typedef struct Source {
    uint32_t va, size;
    PagerSourceFn fn;
    void *ctx;
} Source;

typedef struct Pte {
    uint32_t hi, lo;
} Pte;

static Pte *g_htab;
static uint8_t *g_frames;
static Frame *g_frame;
static uint32_t g_frame_count, g_hand, g_free_frames;
static uint32_t *g_chunks[4096]; /* per 1 MB of the 4 GB: CHUNK_PAGES states */
static uint32_t g_aram_base;
static uint32_t *g_sector_used; /* bitmap of ARAM sectors */
static uint32_t g_sector_count, g_sector_rover;
static void (*g_full)(void);
static Source g_sources[64];
static uint32_t g_source_count;
static PagerStats g_stats;

static inline uint32_t vsid_of(uint32_t va) { return 0x100u + (va >> 28); }

static inline uint64_t timebase(void) {
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0; mftb %1; mftbu %2" : "=r"(hi), "=r"(lo), "=r"(hi2));
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

static inline uint32_t interrupts_on(void) {
    uint32_t msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr | 0x8000u) : "memory"); /* MSR[EE] */
    return msr;
}
static inline void interrupts_restore(uint32_t msr) { __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory"); }

static inline void tlb_invalidate(uint32_t va) {
    __asm__ volatile("sync; tlbie %0; sync; tlbsync; sync" : : "r"(va) : "memory");
}

static inline void pte_flush(Pte *p) {
    DCFlushRangeNoSync(p, sizeof *p);
    __asm__ volatile("sync" : : : "memory");
}

static uint32_t *state_of(uint32_t vpage, int create) {
    uint32_t c = vpage / CHUNK_PAGES;
    if (c >= 4096 || !PAGED_SEGMENT(c >> 8)) return 0;
    if (!g_chunks[c]) {
        if (!create) return 0;
        /* allocated outside the handler only (map calls): the handler never creates chunks */
        g_chunks[c] = (uint32_t *)memalign(32, CHUNK_PAGES * 4);
        memset(g_chunks[c], 0, CHUNK_PAGES * 4);
        DCFlushRange(g_chunks[c], CHUNK_PAGES * 4);
    }
    return &g_chunks[c][vpage % CHUNK_PAGES];
}

static Pte *group(uint32_t va, int secondary) {
    uint32_t hash = (vsid_of(va) & 0x7ffffu) ^ ((va >> 12) & 0xffffu);
    if (secondary) hash = ~hash;
    hash &= (HTAB_BYTES / 64u) - 1u;
    return g_htab + hash * 8u;
}

static uint32_t pte_word0(uint32_t va, int secondary) {
    return 0x80000000u | vsid_of(va) << 7 | (uint32_t)secondary << 6 | ((va >> 22) & 0x3fu);
}

static void frame_release(uint32_t f);

/* Puts frame f's translation for va in the table; a full pair of groups loses an entry
 * (that page's frame stays resident: its next touch faults and re-inserts it). */
static void pte_insert(uint32_t va, uint32_t f, int writable) {
    const uint32_t phys = ((uint32_t)(g_frames + f * PAGE)) & 0x3fffffffu;
    const uint32_t lo = (phys & ~0xfffu) | (writable ? 2u : 3u); /* WIMG 0000: cached, coherent off */
    for (int sec = 0; sec < 2; sec++) {
        Pte *g = group(va, sec);
        for (int i = 0; i < 8; i++) {
            if (g[i].hi & 0x80000000u) continue;
            g[i].lo = lo;
            __asm__ volatile("eieio" : : : "memory");
            g[i].hi = pte_word0(va, sec);
            pte_flush(&g[i]);
            g_frame[f].pte = (uint16_t)((g + i) - g_htab);
            return;
        }
    }
    /* both groups full: displace the primary group's first entry */
    Pte *g = group(va, 0);
    for (uint32_t k = 0; k < g_frame_count; k++)
        if (g_frame[k].vpage != NO_FRAME && g_frame[k].pte == (uint16_t)(g - g_htab)) {
            g_frame[k].pte = 0xffffu;
            tlb_invalidate(g_frame[k].vpage << 12);
            break;
        }
    g->hi = 0;
    __asm__ volatile("eieio" : : : "memory");
    g->lo = lo;
    __asm__ volatile("eieio" : : : "memory");
    g->hi = pte_word0(va, 0);
    pte_flush(g);
    g_frame[f].pte = (uint16_t)(g - g_htab);
}

/* ---- ARAM sectors ---- */
static inline int sector_used(uint32_t i) { return (g_sector_used[i >> 5] >> (i & 31u)) & 1u; }
/* (pages: the count of pages it adds or removes, 1 for a page's sectors, 0 for others') */
static void sectors_mark(uint32_t first, uint32_t n, int used, int pages) {
    for (uint32_t i = first; i < first + n; i++) {
        if (used) g_sector_used[i >> 5] |= 1u << (i & 31u);
        else g_sector_used[i >> 5] &= ~(1u << (i & 31u));
    }
    if (used) g_stats.aram_used += n, g_stats.aram_pages += (uint32_t)pages;
    else g_stats.aram_used -= n, g_stats.aram_pages -= (uint32_t)pages;
}
/* n consecutive free sectors (first fit from the rover), or NO_SLOT */
static uint32_t sectors_alloc(uint32_t n) {
    uint32_t run = 0, start = 0;
    for (uint32_t k = 0; k < g_sector_count + n; k++) {
        uint32_t i = g_sector_rover + k;
        if (i >= g_sector_count) i -= g_sector_count;
        if (i == 0) run = 0; /* (runs do not wrap) */
        if (!(i & 31u) && g_sector_used[i >> 5] == 0xffffffffu && i + 32 <= g_sector_count) {
            run = 0;
            k += 31;
            continue;
        }
        if (sector_used(i)) {
            run = 0;
            continue;
        }
        if (!run) start = i;
        if (++run == n) {
            sectors_mark(start, n, 1, 1);
            g_sector_rover = start + n;
            if (g_sector_rover >= g_sector_count) g_sector_rover = 0;
            return start;
        }
    }
    return NO_SLOT;
}

/* ---- page compression: LZ77 in LZ4's token format, 4 KB at a time ----
 * A sequence: a token (literals 4 bits, match length - 4 in 4 bits, 15: more length bytes
 * follow), the literals, a 2-byte offset back (little end first), the match's extra length
 * bytes. The last sequence has literals only. */
#define LZ_HASH_BITS 11
static uint16_t g_lz_hash[1u << LZ_HASH_BITS];
static uint8_t g_lz_buf[PAGE + 64] __attribute__((aligned(32))); /* compressed, DMA bounce */

static inline uint32_t lz_read32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint8_t *lz_len(uint8_t *o, uint32_t n) {
    while (n >= 255) *o++ = 255, n -= 255;
    *o++ = (uint8_t)n;
    return o;
}
/* compressed size, or 0 when it would not save a sector */
static uint32_t lz_compress(const uint8_t *in, uint8_t *out) {
    const uint32_t limit = PAGE - SECTOR;
    memset(g_lz_hash, 0, sizeof g_lz_hash);
    uint32_t i = 0, anchor = 0;
    uint8_t *o = out;
    while (i + 12 <= PAGE) {
        const uint32_t v = lz_read32(in + i);
        const uint32_t h = (v * 2654435761u) >> (32 - LZ_HASH_BITS);
        const uint32_t cand = g_lz_hash[h];
        g_lz_hash[h] = (uint16_t)i;
        if (cand >= i || i - cand > 0xffffu || lz_read32(in + cand) != v) {
            i++;
            continue;
        }
        uint32_t len = 4;
        while (i + len < PAGE - 5 && in[cand + len] == in[i + len]) len++;
        const uint32_t lit = i - anchor;
        if ((uint32_t)(o - out) + lit + 8 + lit / 255 >= limit) return 0;
        uint8_t *token = o++;
        *token = (uint8_t)((lit >= 15 ? 15 : lit) << 4 | (len - 4 >= 15 ? 15 : len - 4));
        if (lit >= 15) o = lz_len(o, lit - 15);
        memcpy(o, in + anchor, lit);
        o += lit;
        *o++ = (uint8_t)(i - cand), *o++ = (uint8_t)((i - cand) >> 8);
        if (len - 4 >= 15) o = lz_len(o, len - 4 - 15);
        i += len;
        anchor = i;
    }
    const uint32_t lit = PAGE - anchor;
    if ((uint32_t)(o - out) + lit + 2 + lit / 255 >= limit) return 0;
    *o++ = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15) o = lz_len(o, lit - 15);
    memcpy(o, in + anchor, lit);
    o += lit;
    return (uint32_t)(o - out);
}
static void lz_decompress(const uint8_t *in, uint8_t *out) {
    uint8_t *o = out, *const end = out + PAGE;
    for (;;) {
        const uint8_t token = *in++;
        uint32_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do lit += b = *in++; while (b == 255);
        }
        memcpy(o, in, lit);
        o += lit, in += lit;
        if (o >= end) return;
        const uint32_t off = (uint32_t)in[0] | (uint32_t)in[1] << 8;
        in += 2;
        uint32_t len = (token & 15u) + 4;
        if ((token & 15u) == 15) {
            uint8_t b;
            do len += b = *in++; while (b == 255);
        }
        const uint8_t *m = o - off;
        while (len--) *o++ = *m++; /* (overlapping copies repeat) */
    }
}

static void aram_dma(int to_aram, void *mem, uint32_t sector, uint32_t bytes) {
    if (to_aram) DCFlushRange(mem, bytes);
    else DCInvalidateRange(mem, bytes);
    AR_StartDMA(to_aram ? AR_MRAMTOARAM : AR_ARAMTOMRAM, (u32)mem, g_aram_base + sector * SECTOR, bytes);
    while (AR_GetDMAStatus()) {}
}
/* ---- ARAM blocks for others (the GX textures' copies): from the top down, while pages fill
 * from the rover; with interrupts off (the handler allocates and DMAs from any thread) ---- */
uint32_t pager_aram_alloc(uint32_t bytes) {
    const uint32_t n = (bytes + SECTOR - 1) / SECTOR;
    const uint32_t level = IRQ_Disable();
    uint32_t run = 0, found = NO_SLOT;
    for (uint32_t i = g_sector_count; i-- > 0;) {
        if (sector_used(i)) {
            run = 0;
            continue;
        }
        if (++run == n) {
            found = i;
            break;
        }
    }
    if (found != NO_SLOT) sectors_mark(found, n, 1, 0), g_stats.aram_other += n;
    IRQ_Restore(level);
    return found == NO_SLOT ? 0 : g_aram_base + found * SECTOR;
}
void pager_aram_free(uint32_t addr, uint32_t bytes) {
    const uint32_t n = (bytes + SECTOR - 1) / SECTOR;
    const uint32_t level = IRQ_Disable();
    sectors_mark((addr - g_aram_base) / SECTOR, n, 0, 0);
    g_stats.aram_other -= n;
    IRQ_Restore(level);
}
void pager_aram_copy(int to_aram, void *mem, uint32_t addr, uint32_t bytes) {
    /* 16 KB a DMA, interrupts off for each (the handler's DMAs must not overlap one) */
    if (to_aram) DCFlushRange(mem, bytes);
    else DCInvalidateRange(mem, bytes);
    for (uint32_t done = 0; done < bytes; done += 16384u) {
        const uint32_t n = bytes - done < 16384u ? bytes - done : 16384u;
        const uint32_t level = IRQ_Disable();
        AR_StartDMA(to_aram ? AR_MRAMTOARAM : AR_ARAMTOMRAM, (u32)mem + done, addr + done, n);
        while (AR_GetDMAStatus()) {}
        IRQ_Restore(level);
    }
}

/* the page into ARAM: its first sector (and count in *nsec), or NO_SLOT when ARAM is full */
static uint32_t aram_store(const uint8_t *page, uint32_t *nsec) {
    const uint32_t size = lz_compress(page, g_lz_buf);
    const uint32_t n = size ? (size + SECTOR - 1) / SECTOR : PAGE / SECTOR;
    const uint32_t first = sectors_alloc(n);
    if (first == NO_SLOT) return NO_SLOT;
    if (size) aram_dma(1, g_lz_buf, first, n * SECTOR);
    else aram_dma(1, (void *)page, first, PAGE);
    g_stats.aram_bytes_in += PAGE, g_stats.aram_bytes_out += n * SECTOR;
    *nsec = n;
    return first;
}
static void aram_load(uint8_t *page, uint32_t first, uint32_t nsec) {
    if (nsec == PAGE / SECTOR) {
        aram_dma(0, page, first, PAGE);
        return;
    }
    aram_dma(0, g_lz_buf, first, nsec * SECTOR);
    lz_decompress(g_lz_buf, page);
}
static int page_is_zero(const uint8_t *page) {
    const uint32_t *w = (const uint32_t *)page;
    for (uint32_t i = 0; i < PAGE / 4; i += 8)
        if (w[i] | w[i + 1] | w[i + 2] | w[i + 3] | w[i + 4] | w[i + 5] | w[i + 6] | w[i + 7]) return 0;
    return 1;
}

/* Frees frame f: a changed page goes to ARAM first, a clean one keeps its ARAM copy (or is
 * dropped: zeros never written, source pages read again). */
static void frame_evict(uint32_t f) {
    Frame *fr = &g_frame[f];
    uint32_t *st = state_of(fr->vpage, 0);
    int changed = 0;
    if (fr->pte != 0xffffu) {
        Pte *p = g_htab + fr->pte;
        changed = (p->lo & 0x80u) != 0;
        p->hi = 0;
        pte_flush(p);
        tlb_invalidate(fr->vpage << 12);
    }
    uint32_t s = *st;
    if (ST_KIND(s) == K_ZERO) {
        const uint8_t *page = g_frames + f * PAGE;
        if (changed && fr->slot != NO_SLOT) { /* its ARAM copy is stale */
            sectors_mark(fr->slot, fr->nsec, 0, 1);
            fr->slot = NO_SLOT;
        }
        if (fr->slot == NO_SLOT && (!changed || page_is_zero(page))) {
            /* zeros (never written since it was zero-filled, or written back to zeros): it
             * stays a zero page, nothing kept */
            *st = (s & 0xe0000000u);
            if (changed) g_stats.zero_dropped++;
            frame_release(f);
            g_stats.evictions++;
            return;
        }
        if (fr->slot == NO_SLOT) {
            uint32_t n;
            const uint32_t first = aram_store(page, &n);
            if (first == NO_SLOT) {
                /* ARAM full: nowhere to put it (the SD tier is not written yet) */
                g_stats.aram_full++;
                if (g_full) g_full();
                for (;;) {}
            }
            fr->slot = (uint16_t)first, fr->nsec = (uint8_t)n;
            g_stats.writebacks++;
        }
        *st = (s & 0xe0000000u) | ST_ARAM | (uint32_t)(fr->nsec - 1u) << 16 | fr->slot;
    } else {
        /* a source page: read again from its source next time */
        *st = (s & 0xe0000000u) | ((uint32_t)ST_SRC(s) << 16);
    }
    fr->slot = NO_SLOT;
    frame_release(f);
    g_stats.evictions++;
}

static void frame_release(uint32_t f) {
    g_frame[f].vpage = NO_FRAME;
    g_frame[f].pte = 0xffffu;
    g_frame[f].slot = NO_SLOT;
    g_frame[f].nsec = 0;
    g_frame[f].busy = 0;
    g_free_frames++;
}

/* A frame to fill: a free one, else the clock's next unreferenced one. */
static uint32_t frame_take(void) {
    if (g_free_frames) {
        for (uint32_t i = 0; i < g_frame_count; i++) {
            uint32_t f = (g_hand + i) % g_frame_count;
            if (g_frame[f].vpage == NO_FRAME) {
                g_free_frames--;
                return f;
            }
        }
    }
    for (;;) {
        uint32_t f = g_hand;
        g_hand = (g_hand + 1) % g_frame_count;
        Frame *fr = &g_frame[f];
        if (fr->busy) continue;
        if (fr->pte != 0xffffu) {
            Pte *p = g_htab + fr->pte;
            if (p->lo & 0x100u) { /* referenced since the last pass: second chance */
                p->lo &= ~0x100u;
                pte_flush(p);
                tlb_invalidate(fr->vpage << 12);
                continue;
            }
        }
        frame_evict(f);
        g_free_frames--;
        return f;
    }
}

static void (*g_report)(uint32_t, int, int, uint32_t);
void pager_set_report(void (*fn)(uint32_t, int, int, uint32_t)) { g_report = fn; }
void pager_set_full(void (*fn)(void)) { g_full = fn; }

static int unresolved(uint32_t addr, int store, int exec, uint32_t pc) {
    if (g_report) g_report(addr, store, exec, pc);
    return 0;
}

int pager_fault(uint32_t addr, int store, int exec, uint32_t pc) {
    const uint64_t t0 = timebase();
    const uint32_t vpage = addr >> 12;
    uint32_t *st = state_of(vpage, 0);
    if (!st || ST_KIND(*st) == K_UNMAPPED) return unresolved(addr, store, exec, pc);
    uint32_t s = *st;
    if (exec && !(s & ST_EXEC)) return unresolved(addr, store, exec, pc);
    if (exec) g_stats.faults_code++;
    else g_stats.faults_data++;
    const int writable = ST_KIND(s) == K_ZERO;
    if (s & ST_LOADING) {
        /* another thread is filling it: wait (interrupts on, so that it can) and retry */
        const uint32_t msr = interrupts_on();
        while (*(volatile uint32_t *)st & ST_LOADING) usleep(200);
        interrupts_restore(msr);
        g_stats.fault_ticks += timebase() - t0;
        return 1;
    }
    if (s & ST_RESIDENT) {
        /* resident, its entry displaced from a full group */
        pte_insert(vpage << 12, ST_LOW(s), writable);
        g_stats.fault_ticks += timebase() - t0;
        return 1;
    }
    const uint32_t f = frame_take();
    uint8_t *mem = g_frames + f * PAGE;
    g_frame[f].vpage = vpage;
    g_frame[f].slot = NO_SLOT;
    if (s & ST_ARAM) {
        g_frame[f].slot = (uint16_t)ST_LOW(s), g_frame[f].nsec = (uint8_t)ST_NSEC(s);
        aram_load(mem, ST_LOW(s), ST_NSEC(s));
        g_stats.fills_aram++;
    } else if (ST_KIND(s) == K_ZERO) {
        for (uint32_t i = 0; i < PAGE; i += 32) __asm__ volatile("dcbz %0,%1" : : "b"(mem), "r"(i) : "memory");
        g_stats.fills_zero++;
    } else {
        /* the source reads a file (the disc's or the SD card's driver waits on interrupts):
         * interrupts on meanwhile, the frame kept from eviction and the page marked, so that
         * other threads run and a second fault on it waits */
        const Source *src = &g_sources[ST_SRC(s)];
        g_frame[f].busy = 1;
        *st = s | ST_LOADING;
        const uint32_t msr = interrupts_on();
        src->fn(src->ctx, (vpage << 12) - src->va, mem);
        interrupts_restore(msr);
        g_frame[f].busy = 0;
        g_stats.fills_source++;
    }
    if (exec || (s & ST_EXEC)) {
        DCFlushRange(mem, PAGE);
        ICInvalidateRange(mem, PAGE);
    }
    *st = (s & 0xe0000000u) | ST_RESIDENT | (ST_KIND(s) == K_SOURCE ? (uint32_t)ST_SRC(s) << 16 : 0) | f;
    pte_insert(vpage << 12, f, writable);
    g_stats.fault_ticks += timebase() - t0;
    return 1;
}

void pager_init(void *frames, uint32_t frame_bytes, uint32_t aram_base, uint32_t aram_size) {
    g_htab = (Pte *)memalign(HTAB_BYTES, HTAB_BYTES);
    memset(g_htab, 0, HTAB_BYTES);
    DCFlushRange(g_htab, HTAB_BYTES);
    g_frames = (uint8_t *)frames;
    g_frame_count = frame_bytes / PAGE;
    g_frame = (Frame *)malloc(g_frame_count * sizeof(Frame));
    for (uint32_t f = 0; f < g_frame_count; f++) {
        g_frame[f].vpage = NO_FRAME;
        g_frame[f].pte = 0xffffu;
        g_frame[f].slot = NO_SLOT;
        g_frame[f].busy = 0;
    }
    g_free_frames = g_frame_count;
    g_aram_base = (aram_base + 31u) & ~31u;
    g_sector_count = (aram_size - (g_aram_base - aram_base)) / SECTOR;
    if (g_sector_count > SECTORS_MAX) g_sector_count = SECTORS_MAX;
    g_sector_used = (uint32_t *)calloc((g_sector_count + 31) / 32, 4);
    g_stats.frames = g_frame_count;
    g_stats.aram_sectors = g_sector_count;

    /* SDR1: the table's physical address and size mask (HTABMASK covers 128 KB) */
    mtspr(25, ((uint32_t)g_htab & 0x3fffffffu) | ((HTAB_BYTES >> 16) - 1u));
    for (uint32_t seg = 0; seg < 16; seg++)
        if (PAGED_SEGMENT(seg)) __asm__ volatile("mtsrin %0,%1; isync" : : "r"(vsid_of(seg << 28)), "r"(seg << 28));
    __asm__ volatile("sync; isync");
    __exception_sethandler(EX_DSI, pager_dsi_stub);
    __exception_sethandler(EX_ISI, pager_isi_stub);
}

static void map_range(uint32_t va, uint32_t size, uint32_t state) {
    uint32_t level = IRQ_Disable();
    for (uint32_t p = va >> 12; p < (va + size + PAGE - 1) >> 12; p++) *state_of(p, 1) = state;
    IRQ_Restore(level);
}

void pager_map_zero(uint32_t va, uint32_t size) { map_range(va, size, (uint32_t)K_ZERO << 30); }

void pager_map_source(uint32_t va, uint32_t size, int exec, PagerSourceFn fn, void *ctx) {
    uint32_t i = g_source_count++;
    g_sources[i] = (Source){va, size, fn, ctx};
    map_range(va, size, (uint32_t)K_SOURCE << 30 | (exec ? ST_EXEC : 0) | i << 16);
}

static void drop_pages(uint32_t va, uint32_t size, int unmap) {
    uint32_t level = IRQ_Disable();
    for (uint32_t p = (va + PAGE - 1) >> 12; p < (va + size) >> 12; p++) {
        uint32_t *st = state_of(p, 0);
        if (!st) continue;
        uint32_t s = *st;
        if (s & ST_RESIDENT) {
            Frame *fr = &g_frame[ST_LOW(s)];
            if (fr->pte != 0xffffu) {
                g_htab[fr->pte].hi = 0;
                pte_flush(&g_htab[fr->pte]);
                tlb_invalidate(p << 12);
            }
            if (fr->slot != NO_SLOT) sectors_mark(fr->slot, fr->nsec, 0, 1);
            frame_release(ST_LOW(s));
        } else if (s & ST_ARAM) {
            sectors_mark(ST_LOW(s), ST_NSEC(s), 0, 1);
        }
        *st = unmap ? 0 : (s & 0xe0000000u & ~ST_EXEC) | (ST_KIND(s) == K_SOURCE ? (uint32_t)ST_SRC(s) << 16 : 0);
    }
    IRQ_Restore(level);
}

void pager_discard(uint32_t va, uint32_t size) { drop_pages(va, size, 0); }
void pager_unmap(uint32_t va, uint32_t size) { drop_pages(va, size, 1); }

void pager_stats(PagerStats *s) {
    uint32_t level = IRQ_Disable();
    *s = g_stats;
    s->frames_used = g_frame_count - g_free_frames;
    IRQ_Restore(level);
}
