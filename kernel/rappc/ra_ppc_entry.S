# ra_ppc_entry — called from the Starlet-built stub inside the game's
# __VIRetraceHandler (MEM1 0x80001040):
#
#     stw   rY, d(r13)        # the game's retraceCount++ store (moved here)
#     stwu  r1, -16(r1)
#     stw   r0, 8(r1)
#     lis   r0, ra_ppc_entry@h
#     ori   r0, r0, ra_ppc_entry@l
#     mtlr  r0
#     blrl                    # -> here; LR = the stub's last word
#     b     <back into the game>
#
# Interrupts are off (VI interrupt handler). The game's registers must all
# survive: we switch to a private stack, save every volatile register the C
# code may clobber, call ra_ppc_main, restore, and return with r0 and the
# stub's 16-byte frame popped (the stub has no room to do it itself). LR is
# dead at the hook point (the handler saved it in its prologue and calls
# PreCB right after), so the stub may use it.

	.text
	.globl	ra_ppc_entry
ra_ppc_entry:
	stw	11, -4(1)		# r11 -> below the stub frame (EE off: nothing else uses it)
	lis	11, (ra_ppc_stack + 4096 - 96)@ha
	addi	11, 11, (ra_ppc_stack + 4096 - 96)@l
	stw	1, 0(11)		# back chain = the game's SP (= stub frame)
	mr	1, 11			# private stack
	lwz	11, 0(1)
	lwz	11, -4(11)		# original r11
	stw	3,  8(1)
	stw	4,  12(1)
	stw	5,  16(1)
	stw	6,  20(1)
	stw	7,  24(1)
	stw	8,  28(1)
	stw	9,  32(1)
	stw	10, 36(1)
	stw	11, 40(1)
	stw	12, 44(1)
	mflr	3
	stw	3,  48(1)
	mfctr	3
	stw	3,  52(1)
	mfcr	3
	stw	3,  56(1)
	mfxer	3
	stw	3,  60(1)
	stw	2,  64(1)
	stw	13, 68(1)

	bl	ra_ppc_main

	lwz	13, 68(1)
	lwz	2,  64(1)
	lwz	3,  60(1)
	mtxer	3
	lwz	3,  56(1)
	mtcrf	0xFF, 3
	lwz	3,  52(1)
	mtctr	3
	lwz	3,  48(1)
	mtlr	3
	lwz	12, 44(1)
	lwz	11, 40(1)
	lwz	10, 36(1)
	lwz	9,  32(1)
	lwz	8,  28(1)
	lwz	7,  24(1)
	lwz	6,  20(1)
	lwz	5,  16(1)
	lwz	4,  12(1)
	lwz	3,  8(1)
	lwz	1, 0(1)			# back to the stub frame
	lwz	0, 8(1)			# the stub-saved r0
	addi	1, 1, 16		# pop the stub frame
	blr
