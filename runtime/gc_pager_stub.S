/* DSI / ISI entries of the GameCube pager (gc_pager.c).
 *
 * libogc's exception vector has stored r0-r5, CR, LR, CTR, XER, SRR0 and SRR1 in a frame at
 * the stack pointer (asm.h offsets) and jumps here with translation on. These save the other
 * volatile registers, call pager_fault(address, store, exec, pc) and, when it resolved the fault,
 * return to the faulting instruction; otherwise they continue in libogc's default handler
 * (the crash report). After the pattern of VBA-GX's vmdsihandler.S (tueidj).
 */
#include <ppc-asm.h>
#include <ogc/machine/asm.h>

	.extern pager_fault
	.extern default_exceptionhandler

.macro PAGER_ENTRY name, kind
	.globl \name
\name:
	stwu	sp,-EXCEPTION_FRAME_END(sp)
	stw	r6,GPR6_OFFSET(sp)
	stw	r7,GPR7_OFFSET(sp)
	stw	r8,GPR8_OFFSET(sp)
	stw	r9,GPR9_OFFSET(sp)
	stw	r10,GPR10_OFFSET(sp)
	stw	r11,GPR11_OFFSET(sp)
	stw	r12,GPR12_OFFSET(sp)
.if \kind == 0
	mfdar	r3			/* DSI: the data address; DSISR bit 6: a store */
	mfdsisr	r4
	rlwinm	r4,r4,7,31,31
	li	r5,0
	mfdsisr	r6			/* only "no translation" faults are ours */
	rlwinm.	r6,r6,0,1,1
	beq	2f
.else
	lwz	r3,SRR0_OFFSET(sp)	/* ISI: the instruction address; SRR1 bit 1: no translation */
	li	r4,0
	li	r5,1
	lwz	r6,SRR1_OFFSET(sp)
	rlwinm.	r6,r6,0,1,1
	beq	2f
.endif
	mfmsr	r0
	ori	r0,r0,MSR_RI
	mtmsr	r0
	lwz	r6,SRR0_OFFSET(sp)	/* the faulting instruction (reports) */
	bl	pager_fault
	cmplwi	r3,0
	b	3f
2:	li	r3,0
	cmplwi	r3,0
3:	lwz	r6,GPR6_OFFSET(sp)
	lwz	r7,GPR7_OFFSET(sp)
	lwz	r8,GPR8_OFFSET(sp)
	lwz	r9,GPR9_OFFSET(sp)
	lwz	r10,GPR10_OFFSET(sp)
	lwz	r11,GPR11_OFFSET(sp)
	lwz	r12,GPR12_OFFSET(sp)
	mfmsr	r0
	rlwinm	r0,r0,0,31,29		/* clear MSR[RI] */
	mtmsr	r0
	bne	1f
	addi	sp,sp,EXCEPTION_FRAME_END
	b	default_exceptionhandler
1:	lwz	r3,CR_OFFSET(sp)
	lwz	r4,LR_OFFSET(sp)
	lwz	r5,CTR_OFFSET(sp)
	lwz	r0,XER_OFFSET(sp)
	mtcr	r3
	mtlr	r4
	mtctr	r5
	mtxer	r0
	lwz	r0,GPR0_OFFSET(sp)
	lwz	r5,GPR5_OFFSET(sp)
	lwz	r3,SRR0_OFFSET(sp)
	lwz	r4,SRR1_OFFSET(sp)
	mtsrr0	r3
	mtsrr1	r4
	lwz	r3,GPR3_OFFSET(sp)
	lwz	r4,GPR4_OFFSET(sp)
	lwz	sp,GPR1_OFFSET(sp)
	rfi
.endm

	PAGER_ENTRY pager_dsi_stub, 0
	PAGER_ENTRY pager_isi_stub, 1
