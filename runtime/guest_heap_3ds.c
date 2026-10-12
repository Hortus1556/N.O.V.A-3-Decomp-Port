// The guest heap on the New 3DS (docs/3ds.md "Memory layout"): dlmalloc 2.8.6
// (extern/dlmalloc/malloc.c, MIT-0) with the dl prefix, in memory of its own, so that the game's
// blocks never sit next to the host's (newlib's heap: libctru, thread stacks, the runtime's
// containers) and an overrun in the game cannot damage them.
//
// The regular heap's free part is shared out from both ends: newlib's heap grows up from the
// game's image (libsysbase's sbrk, limited by fake_heap_end), the guest heap down from the top
// in 1 MB segments (dlmalloc's MMAP hook, which merges a segment with the one above it). Each
// new guest segment lowers fake_heap_end, under newlib's malloc lock, so the two never meet.
// A console's regular heap is at most 96 MB (OS_HEAP_AREA_END, os_3ds.cpp): the guest stops
// taking it where newlib's budget begins (kHostBudget above the image) and continues in the
// linear heap, leaving the GPU its share there (kGpuReserve: textures, vertex buffers, the
// frame's ring and command list, at most about 34 MB in a level). Measured peaks: newlib ~27 MB,
// the guest ~65 MB (chapter 3), so the guest gets ~38 MB of the regular heap and up to ~41 MB
// of the linear one.
// Nothing goes back: the guest keeps its peak footprint (as newlib keeps its own).
#include <3ds.h>

#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern char *fake_heap_start, *fake_heap_end;
void __malloc_lock(struct _reent *);
void __malloc_unlock(struct _reent *);
void guest_heap_error(const char *what, void *p);  // native_3ds.cpp: a fatal error with the game's callers

static char *g_guest_bottom;                // the lowest address the guest heap has (0: none yet)
static size_t g_failed_size;                // the last segment no heap could give (the out-of-memory report)
static const size_t kHostReserve = 2u << 20;  // what newlib keeps in any case (thread stacks, a log line)
static const size_t kHostBudget = 30u << 20;  // newlib's part of the regular heap above the image (peak ~27 MB)
static const size_t kGpuReserve = 40u << 20;  // linear memory the guest leaves to the GPU (peak ~34 MB)
static size_t g_guest_linear;               // linear memory the guest has taken
extern u32 __ctru_linear_heap_size;

static void *guest_heap_more(size_t size) {
    void *r = (void *)~(size_t)0;  // MFAIL
    __malloc_lock(_REENT);
    if (!g_guest_bottom) g_guest_bottom = fake_heap_end;
    char *brk = (char *)_sbrk_r(_REENT, 0);
    char *floor = fake_heap_start + kHostBudget;
    if (brk != (char *)-1 && g_guest_bottom - brk >= (ptrdiff_t)(size + kHostReserve) &&
        g_guest_bottom - floor >= (ptrdiff_t)size) {
        g_guest_bottom -= size;
        fake_heap_end = g_guest_bottom;
        r = g_guest_bottom;
    }
    __malloc_unlock(_REENT);
    // the guest's share of the linear heap is a cap on its own total (the GPU keeps kGpuReserve
    // of it, whenever it asks), not free space it must leave on top of what the GPU already has
    if (r == (void *)~(size_t)0 && g_guest_linear + size + kGpuReserve <= __ctru_linear_heap_size) {
        void *p = linearMemAlign(size, 0x1000);
        if (p) {
            memset(p, 0, size);  // (MMAP_CLEARS: dlmalloc takes segments as zeroed)
            g_guest_linear += size;
            r = p;
        }
    }
    if (r == (void *)~(size_t)0) g_failed_size = size;
    return r;
}

#define USE_DL_PREFIX 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define LACKS_SYS_MMAN_H 1
#define MMAP(s) guest_heap_more(s)
#define DIRECT_MMAP(s) guest_heap_more(s)  // (not used: no allocation reaches the mmap threshold)
#define MUNMAP(a, s) (-1)                  // segments stay the guest's
#define MMAP_CLEARS 1                      // fresh heap pages are zeroed by the kernel
#define DEFAULT_GRANULARITY ((size_t)1 << 20)
#define DEFAULT_MMAP_THRESHOLD ((size_t)-1)
#define DEFAULT_TRIM_THRESHOLD ((size_t)-1)
#define NO_MALLOC_STATS 1
// blocking locks (libctru's LightLock): a spin lock could spin forever under the kernel's strict
// priorities when the holder has a lower priority on the same core
#define USE_LOCKS 2
#define MLOCK_T LightLock
#define INITIAL_LOCK(lk) (LightLock_Init(lk), 0)
#define DESTROY_LOCK(lk) (0)
#define ACQUIRE_LOCK(lk) (LightLock_Lock(lk), 0)
#define RELEASE_LOCK(lk) LightLock_Unlock(lk)
#define TRY_LOCK(lk) (!LightLock_TryLock(lk))
static MLOCK_T malloc_global_mutex = 1;  // (LightLock: 1 is unlocked)
// a bad free or a damaged chunk header: the game's callers in the log, as bionic's abort would
#define USAGE_ERROR_ACTION(m, p) guest_heap_error("bad free / realloc", (p))
#define CORRUPTION_ERROR_ACTION(m) guest_heap_error("damaged heap", 0)
// out of memory: stop with the game's callers (Android's malloc doesn't fail at these sizes, and
// the game writes through the null pointer it would get: a console then faults far from the cause)
#define MALLOC_FAILURE_ACTION guest_heap_error("out of memory (no segment left in the regular or linear heap; the address is the segment's size)", (void *)g_failed_size)

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../extern/dlmalloc/malloc.c"

// for the memory reports: in use, the footprint (segments taken), its peak
void guest_heap_stats(size_t *live, size_t *footprint, size_t *peak_footprint) {
    struct mallinfo mi = dlmallinfo();
    *live = mi.uordblks;
    *footprint = dlmalloc_footprint();
    *peak_footprint = dlmalloc_max_footprint();
}

// ---- newlib's heap (the host's): checks -----------------------------------------------------
// newlib's malloc is dlmalloc 2.6.4 (mallocr.c): chunks [prev_size, size | PREV_INUSE, fd, bk]
// laid end to end from the start of its sbrk area up to the top chunk, free ones on doubly linked
// lists whose heads are pairs of words in __malloc_av_ (av_[2] is the top chunk). A damaged host
// heap used to show only later, as a fault somewhere inside malloc or free; here every free and
// realloc checks its block (inside the heap, its header whole, still in use: the next chunk's
// PREV_INUSE bit) and every allocation that it lies inside the heap, so the first bad call stops
// with its caller. These read only the block's own two headers and the heap's bounds (its start,
// fake_heap_end), which other threads do not change under it: no lock, a few instructions a call.
// NOVA3_HEAP_CHECK=1 (host_heap_walk) also walks the whole heap and its free lists.
struct nchunk {
    size_t prev_size, size;
    struct nchunk *fd, *bk;
};
extern void *__malloc_av_[258];
extern char *__malloc_sbrk_base;
void *__real__malloc_r(struct _reent *, size_t);
void __real__free_r(struct _reent *, void *);
void *__real__realloc_r(struct _reent *, void *, size_t);
void *__real__memalign_r(struct _reent *, size_t, size_t);
// native_3ds.cpp: logs the damage (the chunks around it) and stops, with the caller of a bad call
__attribute__((noreturn)) void host_heap_error(const char *where, const char *what, const void *block, const void *chunk,
                                               const void *prev, const void *caller);

// where the heap's chunks can be: from its first chunk to fake_heap_end (the top chunk ends below
// it; the guest heap only lowers it), or false before the first allocation
static inline int heap_bounds(char **lo, char **hi) {
    char *base = __malloc_sbrk_base;
    if (base == (char *)-1 || !base) return 0;
    *lo = (char *)(((uintptr_t)base + 7) & ~(uintptr_t)7);
    *hi = fake_heap_end;
    return 1;
}
// a block the caller frees or resizes
static inline void check_block(void *mem, const char *what, const void *caller) {
    char *lo, *hi;
    if (!heap_bounds(&lo, &hi)) return;
    const struct nchunk *p = (const struct nchunk *)((char *)mem - 8);
    if ((uintptr_t)mem & 7 || (char *)p < lo || (char *)p + 16 > hi) host_heap_error(what, "not a block of the heap", mem, 0, 0, caller);
    const size_t size = p->size & ~(size_t)3;
    const struct nchunk *next = (const struct nchunk *)((char *)p + size);
    if (size < 16 || size & 7 || (char *)next + 8 > hi || !(next->size & 1))
        host_heap_error(what, "a damaged header, or freed already", mem, p, 0, caller);
}
// an allocation's result: inside the heap, as large as asked
static inline void *check_result(void *mem, size_t n, const char *what, const void *caller) {
    char *lo, *hi;
    if (!mem || !heap_bounds(&lo, &hi)) return mem;
    const struct nchunk *p = (const struct nchunk *)((char *)mem - 8);
    if ((uintptr_t)mem & 7 || (char *)p < lo || (char *)mem + n > hi || (p->size & ~(size_t)3) < n + 4)
        host_heap_error(what, "a block outside the heap", mem, p, 0, caller);
    return mem;
}

unsigned host_heap_calls;  // allocations and frees (statistics: counted without a lock)
void __wrap__free_r(struct _reent *r, void *mem) {
    if (!mem) return;
    host_heap_calls++;
    check_block(mem, "free", __builtin_return_address(0));
    __real__free_r(r, mem);
}
void *__wrap__malloc_r(struct _reent *r, size_t n) {
    host_heap_calls++;
    return check_result(__real__malloc_r(r, n), n, "malloc", __builtin_return_address(0));
}
void *__wrap__realloc_r(struct _reent *r, void *old, size_t n) {
    host_heap_calls++;
    if (old) check_block(old, "realloc", __builtin_return_address(0));
    return check_result(__real__realloc_r(r, old, n), n, "realloc", __builtin_return_address(0));
}
void *__wrap__memalign_r(struct _reent *r, size_t align, size_t n) {
    host_heap_calls++;
    return check_result(__real__memalign_r(r, align, n), n, "memalign", __builtin_return_address(0));
}
// the checks' own time (a free's and an allocation's, on a block of the heap), for the log
unsigned host_heap_check_ticks(void) {
    void *b = malloc(64);
    const u64 t0 = svcGetSystemTick();
    for (int i = 0; i < 1000; i++) {
        __asm__ volatile("" : "+r"(b) : : "memory");  // (nothing hoisted out of the loop)
        check_block(b, "free", 0);
        check_result(b, 64, "malloc", 0);
    }
    const unsigned t = (unsigned)((svcGetSystemTick() - t0) / 1000);
    free(b);
    return t;
}

// a free list link: another chunk of the heap, or a list head in __malloc_av_
static int link_ok(const void *q, char *lo, char *hi) {
    const char *c = (const char *)q, *av = (const char *)__malloc_av_;
    return (c >= lo && c + 16 <= hi && !((uintptr_t)c & 7)) || (c >= av - 8 && c < av + sizeof(void *) * 258);
}
// The whole heap: every chunk's size, each free chunk's links (and that its neighbours point back)
// and its copy of its size at its end; then each free list. A damaged heap stops with a report.
int host_heap_walk(const char *where) {
    __malloc_lock(_REENT);
    char *lo, *hi;
    const struct nchunk *top = (const struct nchunk *)__malloc_av_[2], *prev = 0;
    if (!heap_bounds(&lo, &hi) || (char *)top < lo) {
        __malloc_unlock(_REENT);
        return 0;
    }
    hi = (char *)top + (top->size & ~(size_t)3);  // (the lock held: the top chunk's end)
    const char *bad = 0;
    for (const struct nchunk *p = (const struct nchunk *)lo; p != top && !bad; prev = p, p = (const struct nchunk *)((char *)p + (p->size & ~(size_t)3))) {
        const size_t size = p->size & ~(size_t)3;
        if ((char *)p + 16 > hi || size < 16 || size & 7 || (char *)p + size > (char *)top) {
            bad = "a chunk's size";
            host_heap_error(where, bad, 0, p, prev, 0);
        }
        const struct nchunk *next = (const struct nchunk *)((char *)p + size);
        if (!(next->size & 1)) {  // p is free
            if (!link_ok(p->fd, lo, hi) || !link_ok(p->bk, lo, hi) || p->fd->bk != p || p->bk->fd != p) bad = "a free chunk's links";
            else if (next->prev_size != size) bad = "a free chunk's size at its end";
            if (bad) host_heap_error(where, bad, 0, p, prev, 0);
        }
    }
    for (int i = 1; i < 128 && !bad; i++) {  // the lists: bin i's head is av_[2i + 2] - 8 (bin 0's "fd" is the top)
        const struct nchunk *head = (const struct nchunk *)((char *)&__malloc_av_[2 * i + 2] - 8), *q = head->fd;
        for (int n = 0; q != head && !bad; q = q->fd, n++) {
            if (!link_ok(q, lo, hi) || (char *)q < lo || n > 1000000) bad = "a free list";
            else if (q->fd->bk != q) bad = "a free list's links";
            if (bad) host_heap_error(where, bad, 0, q, 0, 0);
        }
    }
    __malloc_unlock(_REENT);
    return bad != 0;
}
