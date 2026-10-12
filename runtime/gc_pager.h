/* GameCube demand pager (docs/gamecube.md, "Memory tiers").
 *
 * Virtual memory below 0x80000000, and the top segment (0xf0000000 up), is mapped through the
 * Gekko's hashed page table onto a pool of 4 KB MEM1 frames. A missing page raises a DSI (data) or ISI (instruction) exception;
 * the handler finds a frame (evicting the least recently used one, second chance on the page
 * table's referenced bit), writes an evicted page that was changed to ARAM (compressed; a page
 * of zeros becomes a zero page again), and fills the
 * frame from ARAM, from zeros, or from a range's source (the disc: code, file data).
 *
 * The handler runs in the faulting thread's exception context with interrupts off; it reads
 * ARAM by polled DMA and touches only MEM1 (the host's BAT-mapped memory), never paged memory.
 * Only a source's fill (a file read, which waits on the drive's interrupts) runs with interrupts
 * on: the frame is kept from eviction meanwhile and a second fault on that page waits for it.
 * A source must therefore not be read with interrupts off: code that disables them must not
 * touch source-backed pages it has not touched just before.
 * Thread stacks must therefore be in MEM1: libogc's exception entry stores the registers at
 * the stack pointer's physical address.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills one 4 KB page of a source range: `offset` is the page's byte offset in the range. */
typedef void (*PagerSourceFn)(void *ctx, uint32_t offset, void *page);

/* frames: 4 KB-aligned MEM1 for the resident pages; aram_base / aram_size: the ARAM the pager
 * may use for evicted pages (32-byte aligned). Installs the DSI and ISI handlers. */
void pager_init(void *frames, uint32_t frame_bytes, uint32_t aram_base, uint32_t aram_size);

/* Anonymous read-write memory, zero on first touch (the guest heap, stacks, data). */
void pager_map_zero(uint32_t va, uint32_t size);
/* Read-only memory filled from a source; exec: the CPU may run code from it. */
void pager_map_source(uint32_t va, uint32_t size, int exec, PagerSourceFn fn, void *ctx);
/* The contents of whole pages in [va, va + size) are no longer needed (freed memory): their
 * frames and ARAM copies are released, and the next touch sees zeros. */
void pager_discard(uint32_t va, uint32_t size);
/* Unmaps [va, va + size): a later touch is a fault the pager does not resolve. */
void pager_unmap(uint32_t va, uint32_t size);

typedef struct PagerStats {
    uint32_t frames, frames_used;
    uint32_t faults_data, faults_code;
    uint32_t fills_zero, fills_aram, fills_source;
    uint32_t evictions, writebacks;
    uint32_t zero_dropped;           /* changed pages evicted as zeros: kept as zero pages */
    uint32_t aram_sectors, aram_used; /* ARAM in 256-byte sectors */
    uint32_t aram_pages;             /* pages held there */
    uint32_t aram_other;             /* sectors of pager_aram_alloc's blocks */
    uint32_t aram_full;
    uint64_t aram_bytes_in, aram_bytes_out; /* written: pages, sectors (the compression) */
    uint64_t fault_ticks;  /* time base ticks spent in the handler */
} PagerStats;
void pager_stats(PagerStats *s);

/* The handler's entry (gc_pager_stub.S calls it): 1 resolved, 0 not ours (libogc's default
 * handler takes it, after the report function, if one is set). */
int pager_fault(uint32_t addr, int store, int exec, uint32_t pc);
/* Called (in the exception) for a fault the pager does not resolve, before libogc's handler. */
void pager_set_report(void (*fn)(uint32_t addr, int store, int exec, uint32_t pc));
/* Called (in the exception) when ARAM has no room for a page to evict; the fault never returns. */
void pager_set_full(void (*fn)(void));

/* ARAM for others (the textures' copies), shared with the pages (blocks from the top of ARAM):
 * an ARAM address (32-byte aligned), 0 when there is no room. */
uint32_t pager_aram_alloc(uint32_t bytes);
void pager_aram_free(uint32_t addr, uint32_t bytes);
/* MEM1 <-> ARAM (32-byte aligned addresses, sizes a multiple of 32), cache handled. */
void pager_aram_copy(int to_aram, void *mem, uint32_t addr, uint32_t bytes);

#ifdef __cplusplus
}
#endif
