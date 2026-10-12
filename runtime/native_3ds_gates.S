@ The New 3DS's native code: the assembly between the game's own ARM code and the runtime
@ (runtime/native_3ds.cpp, docs/3ds.md "Native execution").
@
@  * import gates: one per import slot; the game's PLT / GOT entries point at them. The common
@    gate stores the caller's registers into the calling thread's Cpu (r0-r12, sp at the call,
@    lr; cpu->ret_pc = lr), calls rt_call_import(cpu, slot) and returns r0-r3 from the Cpu to
@    the caller. Host C code keeps r4-r11 and d8-d15 (AAPCS), so nothing else is restored; the
@    return address stays on this stack (a nested guest call reuses the same Cpu).
@  * hook gates: the same for the game functions the runtime replaces or wraps (OVERRIDES); the
@    function's first two words are `ldr pc, [pc, #-4]` + the gate's address.
@  * rt_native_enter: host -> game calls (rt::call, a wrap's original): r0-r3 from the Cpu, the
@    stack arguments copied onto this stack, the results back into the Cpu.
@  * setjmp / longjmp on the game's jmp_buf; the Linux kernel user helpers that the Thumb-1
@    Boost.Thread code and libgcc call (their literal-pool addresses are redirected here).
@  * the kernel routine run through svcBackdoor to clean the data cache and invalidate the
@    instruction cache after the loader has written code.
    .arch armv6k
    .fpu vfpv2
    .syntax unified
    .arm
    .text

    .equ CPU_R4, 16
    .equ CPU_R12, 48
    .equ CPU_SP, 52
    .equ CPU_LR, 56
    .equ CPU_RET_PC, 84         @ Cpu: r[16], n, z, c, v, fpscr, ret_pc (runtime/recomp.h)

@ Stores the caller's state into this thread's Cpu and calls `handler(cpu, r12)`; returns to the
@ caller with r0-r3 from the Cpu. Entered with r12 = the gate's number, lr = the return address.
    .macro GATE_COMMON name, handler
    .global \name
    .type \name, %function
\name:
    push    {r0-r3}             @ the arguments
    push    {r4, r5, r12, lr}   @ (8-byte aligned: 32 bytes in all)
    bl      rt_native_cpu       @ r0 = this thread's Cpu (keeps r4-r11)
    mov     r4, r0
    add     r5, sp, #16
    ldm     r5, {r0-r3}
    stm     r4, {r0-r3}         @ cpu->r[0..3]
    ldr     r0, [sp]            @ the caller's r4, r5 (saved above)
    ldr     r1, [sp, #4]
    add     r2, r4, #CPU_R4
    stm     r2, {r0, r1, r6-r11}  @ cpu->r[4..11]
    ldr     r1, [sp, #8]        @ the gate's number
    str     r1, [r4, #CPU_R12]
    add     r2, sp, #32         @ the caller's sp: its stack arguments are there
    str     r2, [r4, #CPU_SP]
    ldr     r3, [sp, #12]
    str     r3, [r4, #CPU_LR]
    str     r3, [r4, #CPU_RET_PC]
    mov     r0, r4
    bl      \handler
    ldm     r4, {r0-r3}         @ the results
    pop     {r4, r5, r12, lr}
    add     sp, sp, #16
    bx      lr
    .size \name, . - \name
    .endm

    GATE_COMMON rt_gate_common, rt_native_import
    GATE_COMMON rt_hook_common, rt_native_hook

@ 2048 import gates (kMaxImportSlots), 12 bytes each: gate i is rt_native_gates + 12 * i.
    .global rt_native_gates
    .balign 4
rt_native_gates:
    .set i, 0
    .rept 2048
    mov     r12, #(i & 0x700)
    orr     r12, r12, #(i & 0xff)
    b       rt_gate_common
    .set i, i + 1
    .endr

@ 256 hook gates, 8 bytes each: hook k's gate is rt_native_hook_gates + 8 * k.
    .global rt_native_hook_gates
    .balign 4
rt_native_hook_gates:
    .set i, 0
    .rept 256
    mov     r12, #i
    b       rt_hook_common
    .set i, i + 1
    .endr

@ void rt_native_enter(Cpu *cpu, uint32_t target, const uint32_t *stack_args, uint32_t count)
@ Calls game code at `target` (bit 0: Thumb) with r0-r3 from the Cpu and `count` stack
@ arguments; r0-r3 go back into the Cpu.
    .global rt_native_enter
    .type rt_native_enter, %function
rt_native_enter:
    push    {r4-r11, lr}
    sub     sp, sp, #4          @ 8-byte aligned
    mov     r4, r0
    mov     r5, sp
    sub     sp, sp, r3, lsl #2
    bic     sp, sp, #7
    mov     r6, #0
1:  cmp     r6, r3
    ldrlo   r7, [r2, r6, lsl #2]
    strlo   r7, [sp, r6, lsl #2]
    addlo   r6, r6, #1
    blo     1b
    mov     r12, r1
    ldm     r4, {r0-r3}
    blx     r12
    mov     sp, r5
    stm     r4, {r0-r3}
    add     sp, sp, #4
    pop     {r4-r11, pc}
    .size rt_native_enter, . - rt_native_enter

@ setjmp / longjmp for the game (Lua's error handling): r4-r11, sp, lr, then d8-d15, in the
@ game's own jmp_buf (bionic's is 256 bytes; this uses 104).
    .global rt_native_setjmp
    .type rt_native_setjmp, %function
rt_native_setjmp:
    mov     r12, sp
    stmia   r0!, {r4-r11, r12, lr}
    vstmia  r0, {d8-d15}
    mov     r0, #0
    bx      lr
    .size rt_native_setjmp, . - rt_native_setjmp

    .global rt_native_longjmp
    .type rt_native_longjmp, %function
rt_native_longjmp:
    ldmia   r0!, {r4-r11, r12, lr}
    vldmia  r0, {d8-d15}
    mov     sp, r12
    movs    r0, r1
    moveq   r0, #1
    bx      lr
    .size rt_native_longjmp, . - rt_native_longjmp

@ Linux kernel user helpers (arch/arm/kernel/entry-armv.S), ARMv6K versions.
@ __kuser_cmpxchg(oldval r0, newval r1, ptr r2): r0 = 0 and C set if *ptr was oldval and is now
@ newval; clobbers r3, ip and the flags.
    .global rt_kuser_cmpxchg
    .type rt_kuser_cmpxchg, %function
rt_kuser_cmpxchg:
    mov     r12, #0
    mcr     p15, 0, r12, c7, c10, 5     @ data memory barrier
1:  ldrex   r3, [r2]
    subs    r3, r3, r0
    strexeq r3, r1, [r2]
    teqeq   r3, #1
    beq     1b
    rsbs    r0, r3, #0
    mcr     p15, 0, r12, c7, c10, 5
    bx      lr
    .size rt_kuser_cmpxchg, . - rt_kuser_cmpxchg

@ __kuser_memory_barrier: no register changes
    .global rt_kuser_barrier
    .type rt_kuser_barrier, %function
rt_kuser_barrier:
    str     r0, [sp, #-4]!
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 5
    ldr     r0, [sp], #4
    bx      lr
    .size rt_kuser_barrier, . - rt_kuser_barrier

@ __kuser_get_tls: r0 = the thread's TLS block (rt::current_tls); nothing else changes
    .global rt_kuser_get_tls
    .type rt_kuser_get_tls, %function
rt_kuser_get_tls:
    push    {r1-r3, r12, lr}
    mrs     r1, apsr
    push    {r1}
    bl      rt_native_tls
    pop     {r1}
    msr     apsr_nzcvq, r1
    pop     {r1-r3, r12, lr}
    bx      lr
    .size rt_kuser_get_tls, . - rt_kuser_get_tls

@ __kuser_helper_version (a word, not code): 5 = __kuser_cmpxchg64 exists
    .global rt_kuser_version
    .balign 4
rt_kuser_version:
    .word   3

@ Run in supervisor mode by svcBackdoor after the loader wrote code: cleans the data cache and
@ invalidates the instruction cache and branch predictor of this core.
    .global rt_native_flush_caches_kernel
    .type rt_native_flush_caches_kernel, %function
rt_native_flush_caches_kernel:
    mov     r0, #0
    mcr     p15, 0, r0, c7, c10, 0      @ clean the entire data cache
    mcr     p15, 0, r0, c7, c10, 4      @ data synchronization barrier
    mcr     p15, 0, r0, c7, c5, 0       @ invalidate the entire instruction cache
    mcr     p15, 0, r0, c7, c5, 6       @ flush the branch target cache
    mcr     p15, 0, r0, c7, c5, 4       @ prefetch flush
    bx      lr
    .size rt_native_flush_caches_kernel, . - rt_native_flush_caches_kernel

@ Run by svcBackdoor: this core's system control register (SCTLR) into rt_native_sctlr. With
@ `enable`, first sets U (bit 22: ARMv6's unaligned access, as ARMv7 always does it; clear, an
@ unaligned ldr returns a rotated word without a fault) and clears A (bit 1: alignment faults).
@ The game is ARMv7 code and does unaligned loads and stores.
    .global rt_native_sctlr_kernel
    .type rt_native_sctlr_kernel, %function
rt_native_sctlr_kernel:
    mrc     p15, 0, r0, c1, c0, 0
    b       1f
    .global rt_native_unaligned_kernel
    .type rt_native_unaligned_kernel, %function
rt_native_unaligned_kernel:
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #(1 << 22)
    bic     r0, r0, #2
    mcr     p15, 0, r0, c1, c0, 0
    mov     r1, #0
    mcr     p15, 0, r1, c7, c5, 4       @ prefetch flush
    mrc     p15, 0, r0, c1, c0, 0
1:  ldr     r1, =rt_native_sctlr
    str     r0, [r1]
    bx      lr
    .ltorg
    .size rt_native_unaligned_kernel, . - rt_native_unaligned_kernel
