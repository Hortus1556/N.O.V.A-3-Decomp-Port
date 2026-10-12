/* stdout / stderr to the IPL device's UART (EXI channel 0, device 1, register 0x20010000),
 * which Dolphin shows as OSReport output (tools/gc_run.py). libogc2's own report stream
 * writes there only on development consoles; on a retail console these writes do nothing.
 * Installed as newlib's devices for descriptors 1 and 2, so every thread's stdout / stderr
 * (each thread has its own newlib streams) ends up here.
 *
 * Also kept in a 64 KB ring that gc_report_flush() appends to a file (gc_report_open: the SD
 * card's log on a console, where the UART goes nowhere); the oldest output is dropped when the
 * ring is full between two flushes. */
#include "gc_report.h"

#include <gccore.h>
#include <ogc/exi.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <unistd.h>

#define RING (64u * 1024u)
static char g_ring[RING];
static uint32_t g_head, g_tail; /* written, flushed (counters: positions modulo RING) */
static int g_fd = -1;
static mutex_t g_flush_lock = LWP_MUTEX_NULL;

static void ring_write(const char *buf, size_t size) {
    const uint32_t level = IRQ_Disable();
    for (size_t i = 0; i < size; i++) g_ring[g_head++ % RING] = buf[i];
    if (g_head - g_tail > RING) g_tail = g_head - RING;
    IRQ_Restore(level);
}

static void uart_write(const char *buf, size_t size) {
    const int n = (int)size;
    if (!EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_1, NULL)) return;
    for (int done = 0; done < n;) {
        char chunk[16];
        int k = 0;
        while (k < 12 && done < n) {
            char c = buf[done++];
            chunk[k++] = c == '\n' ? '\r' : c;
        }
        if (!EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_1, EXI_SPEED8MHZ)) break;
        u32 reg = 0xa0010000u; /* write to the UART register */
        EXI_ImmEx(EXI_CHANNEL_0, &reg, 4, EXI_WRITE);
        EXI_ImmEx(EXI_CHANNEL_0, chunk, k, EXI_WRITE);
        EXI_Deselect(EXI_CHANNEL_0);
    }
    EXI_Unlock(EXI_CHANNEL_0);
}

static ssize_t uart_write_r(struct _reent *r, void *fd, const char *ptr, size_t len) {
    (void)r, (void)fd;
    uart_write(ptr, len);
    ring_write(ptr, len);
    return (ssize_t)len;
}

static const devoptab_t g_uart = {
    .name = "uart",
    .write_r = uart_write_r,
};

void gc_report_install(void) {
    fflush(stdout);
    fflush(stderr);
    devoptab_list[STD_OUT] = &g_uart;
    devoptab_list[STD_ERR] = &g_uart;
    setvbuf(stdout, NULL, _IOLBF, 256);
    setvbuf(stderr, NULL, _IOLBF, 256);
}

void gc_report_raw(const char *s) {
    uart_write(s, strlen(s));
    ring_write(s, strlen(s));
}

int gc_report_open(const char *path) {
    if (g_flush_lock == LWP_MUTEX_NULL) LWP_MutexInit(&g_flush_lock, true);
    g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    return g_fd >= 0;
}

void gc_report_flush(void) {
    if (g_fd < 0) return;
    LWP_MutexLock(g_flush_lock);
    static char chunk[4096];
    for (;;) {
        const uint32_t level = IRQ_Disable();
        uint32_t n = g_head - g_tail;
        if (n > sizeof chunk) n = sizeof chunk;
        for (uint32_t i = 0; i < n; i++) chunk[i] = g_ring[(g_tail + i) % RING];
        g_tail += n;
        IRQ_Restore(level);
        if (!n) break;
        if (write(g_fd, chunk, n) != (ssize_t)n) break;
    }
    fsync(g_fd);
    LWP_MutexUnlock(g_flush_lock);
}

void gc_report_flush_now(void) {
    /* (from an exception handler too: the card's driver waits on interrupts) */
    uint32_t msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr | 0x8000u) : "memory");
    gc_report_flush();
    __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory");
}
