/* GCC's out-of-line 64-bit atomics (libatomic's ABI) for the GameCube: the 32-bit Gekko has no
 * 64-bit atomic instructions and devkitPPC ships no libatomic. One core: interrupts off around
 * the access makes it atomic. Used by the runtime's 64-bit counters (std::atomic<uint64_t>); the
 * translated code's hot paths use 32-bit atomics (recomp.h). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t irq_off(void) {
    uint32_t msr, off;
    __asm__ volatile("mfmsr %0; rlwinm %1,%0,0,17,15; mtmsr %1" : "=r"(msr), "=&r"(off) : : "memory");
    return msr;
}
static inline void irq_restore(uint32_t msr) { __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory"); }

uint64_t __atomic_load_8(const volatile void *mem, int model) {
    (void)model;
    const uint32_t m = irq_off();
    const uint64_t v = *(const volatile uint64_t *)mem;
    irq_restore(m);
    return v;
}

void __atomic_store_8(volatile void *mem, uint64_t val, int model) {
    (void)model;
    const uint32_t m = irq_off();
    *(volatile uint64_t *)mem = val;
    irq_restore(m);
}

uint64_t __atomic_exchange_8(volatile void *mem, uint64_t val, int model) {
    (void)model;
    const uint32_t m = irq_off();
    const uint64_t old = *(volatile uint64_t *)mem;
    *(volatile uint64_t *)mem = val;
    irq_restore(m);
    return old;
}

bool __atomic_compare_exchange_8(volatile void *mem, void *expected, uint64_t desired, bool weak, int success,
                                 int failure) {
    (void)weak, (void)success, (void)failure;
    const uint32_t m = irq_off();
    const uint64_t now = *(volatile uint64_t *)mem;
    uint64_t want;
    memcpy(&want, expected, 8);
    const bool ok = now == want;
    if (ok) *(volatile uint64_t *)mem = desired;
    else memcpy(expected, &now, 8);
    irq_restore(m);
    return ok;
}

#define FETCH_OP(name, op)                                                    \
    uint64_t __atomic_fetch_##name##_8(volatile void *mem, uint64_t val, int model) { \
        (void)model;                                                          \
        const uint32_t m = irq_off();                                         \
        const uint64_t old = *(volatile uint64_t *)mem;                       \
        *(volatile uint64_t *)mem = old op val;                               \
        irq_restore(m);                                                       \
        return old;                                                           \
    }
FETCH_OP(add, +)
FETCH_OP(sub, -)
FETCH_OP(and, &)
FETCH_OP(or, |)
FETCH_OP(xor, ^)
