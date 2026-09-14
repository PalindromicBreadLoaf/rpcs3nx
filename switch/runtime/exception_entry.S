#include "exception_entry.h"

#define SVC_RETURN_FROM_EXCEPTION 0x28
#define RESULT_NOT_HANDLED 0xf801

.section .text.__libnx_exception_entry, "ax", %progbits
.balign 16
.global __libnx_exception_entry
.type __libnx_exception_entry, %function
__libnx_exception_entry:
	cbz x1, .Ldecline

	adrp x2, s_slots_in_use
	add x2, x2, :lo12:s_slots_in_use
.Lclaim:
	ldaxr w3, [x2]
	mvn w4, w3
	and w4, w4, #RPCS3_EXCEPTION_SLOT_MASK
	cbz w4, .Ldecline_locked
	neg w5, w4
	and w5, w5, w4
	orr w6, w3, w5
	stlxr w7, w6, [x2]
	cbnz w7, .Lclaim

	rbit w6, w5
	clz w6, w6

	adrp x4, s_slots
	add x4, x4, :lo12:s_slots
	add x4, x4, x6, lsl #RPCS3_EXCEPTION_SLOT_SHIFT
	str x1, [x4, #RPCS3_SLOT_FRAME]
	str x6, [x4, #RPCS3_SLOT_INDEX]

	str w0, [x4, #RPCS3_DUMP_ERROR_DESC]
	stp wzr, wzr, [x4, #4]
	str wzr, [x4, #12]

	ldp x3, x5, [x1, #0x00]
	stp x3, x5, [x4, #(RPCS3_DUMP_GPRS + 0x00)]
	ldp x3, x5, [x1, #0x10]
	stp x3, x5, [x4, #(RPCS3_DUMP_GPRS + 0x10)]
	ldp x3, x5, [x1, #0x20]
	stp x3, x5, [x4, #(RPCS3_DUMP_GPRS + 0x20)]
	ldp x3, x5, [x1, #0x30]
	stp x3, x5, [x4, #(RPCS3_DUMP_GPRS + 0x30)]
	ldr x3, [x1, #0x40]
	str x3, [x4, #(RPCS3_DUMP_GPRS + 0x40)]

	stp x9, x10, [x4, #(RPCS3_DUMP_GPRS + 0x48)]
	stp x11, x12, [x4, #(RPCS3_DUMP_GPRS + 0x58)]
	stp x13, x14, [x4, #(RPCS3_DUMP_GPRS + 0x68)]
	stp x15, x16, [x4, #(RPCS3_DUMP_GPRS + 0x78)]
	stp x17, x18, [x4, #(RPCS3_DUMP_GPRS + 0x88)]
	stp x19, x20, [x4, #(RPCS3_DUMP_GPRS + 0x98)]
	stp x21, x22, [x4, #(RPCS3_DUMP_GPRS + 0xa8)]
	stp x23, x24, [x4, #(RPCS3_DUMP_GPRS + 0xb8)]
	stp x25, x26, [x4, #(RPCS3_DUMP_GPRS + 0xc8)]
	stp x27, x28, [x4, #(RPCS3_DUMP_GPRS + 0xd8)]
	str x29, [x4, #RPCS3_DUMP_FP]

	ldr x3, [x1, #RPCS3_FRAME_LR]
	str x3, [x4, #RPCS3_DUMP_LR]
	ldr x3, [x1, #RPCS3_FRAME_SP]
	str x3, [x4, #RPCS3_DUMP_SP]
	ldr x3, [x1, #RPCS3_FRAME_PC]
	str x3, [x4, #RPCS3_DUMP_PC]
	str xzr, [x4, #RPCS3_DUMP_PADDING]
	ldp x3, x5, [x1, #RPCS3_FRAME_PSTATE]
	add x7, x4, #RPCS3_DUMP_PSTATE
	stp x3, x5, [x7]
	ldr x3, [x1, #RPCS3_FRAME_FAR]
	str x3, [x4, #RPCS3_DUMP_FAR]

	add x3, x4, #RPCS3_DUMP_FPU
	stp q0, q1, [x3, #0x000]
	stp q2, q3, [x3, #0x020]
	stp q4, q5, [x3, #0x040]
	stp q6, q7, [x3, #0x060]
	stp q8, q9, [x3, #0x080]
	stp q10, q11, [x3, #0x0a0]
	stp q12, q13, [x3, #0x0c0]
	stp q14, q15, [x3, #0x0e0]
	stp q16, q17, [x3, #0x100]
	stp q18, q19, [x3, #0x120]
	stp q20, q21, [x3, #0x140]
	stp q22, q23, [x3, #0x160]
	stp q24, q25, [x3, #0x180]
	stp q26, q27, [x3, #0x1a0]
	stp q28, q29, [x3, #0x1c0]
	stp q30, q31, [x3, #0x1e0]

	add x5, x6, #1
	adrp x3, s_slot_stacks
	add x3, x3, :lo12:s_slot_stacks
	add x3, x3, x5, lsl #RPCS3_EXCEPTION_STACK_SHIFT
	mov sp, x3

	mov x19, x4
	mov x0, x4
	bl HorizonExceptionDispatch
	mov x4, x19
	ldr x1, [x4, #RPCS3_SLOT_FRAME]
	cbz w0, .Lcrash

	ldp x2, x3, [x4, #(RPCS3_DUMP_GPRS + 0x00)]
	stp x2, x3, [x1, #0x00]
	ldp x2, x3, [x4, #(RPCS3_DUMP_GPRS + 0x10)]
	stp x2, x3, [x1, #0x10]
	ldp x2, x3, [x4, #(RPCS3_DUMP_GPRS + 0x20)]
	stp x2, x3, [x1, #0x20]
	ldp x2, x3, [x4, #(RPCS3_DUMP_GPRS + 0x30)]
	stp x2, x3, [x1, #0x30]
	ldr x2, [x4, #(RPCS3_DUMP_GPRS + 0x40)]
	str x2, [x1, #0x40]
	ldr x2, [x4, #RPCS3_DUMP_LR]
	str x2, [x1, #RPCS3_FRAME_LR]
	ldr x2, [x4, #RPCS3_DUMP_SP]
	str x2, [x1, #RPCS3_FRAME_SP]
	ldr x2, [x4, #RPCS3_DUMP_PC]
	str x2, [x1, #RPCS3_FRAME_PC]
	ldr w2, [x4, #RPCS3_DUMP_PSTATE]
	str w2, [x1, #RPCS3_FRAME_PSTATE]

	add x3, x4, #RPCS3_DUMP_FPU
	ldp q0, q1, [x3, #0x000]
	ldp q2, q3, [x3, #0x020]
	ldp q4, q5, [x3, #0x040]
	ldp q6, q7, [x3, #0x060]
	ldp q8, q9, [x3, #0x080]
	ldp q10, q11, [x3, #0x0a0]
	ldp q12, q13, [x3, #0x0c0]
	ldp q14, q15, [x3, #0x0e0]
	ldp q16, q17, [x3, #0x100]
	ldp q18, q19, [x3, #0x120]
	ldp q20, q21, [x3, #0x140]
	ldp q22, q23, [x3, #0x160]
	ldp q24, q25, [x3, #0x180]
	ldp q26, q27, [x3, #0x1a0]
	ldp q28, q29, [x3, #0x1c0]
	ldp q30, q31, [x3, #0x1e0]

	ldp x9, x10, [x4, #(RPCS3_DUMP_GPRS + 0x48)]
	ldp x11, x12, [x4, #(RPCS3_DUMP_GPRS + 0x58)]
	ldp x13, x14, [x4, #(RPCS3_DUMP_GPRS + 0x68)]
	ldp x15, x16, [x4, #(RPCS3_DUMP_GPRS + 0x78)]
	ldp x17, x18, [x4, #(RPCS3_DUMP_GPRS + 0x88)]
	ldp x19, x20, [x4, #(RPCS3_DUMP_GPRS + 0x98)]
	ldp x21, x22, [x4, #(RPCS3_DUMP_GPRS + 0xa8)]
	ldp x23, x24, [x4, #(RPCS3_DUMP_GPRS + 0xb8)]
	ldp x25, x26, [x4, #(RPCS3_DUMP_GPRS + 0xc8)]
	ldp x27, x28, [x4, #(RPCS3_DUMP_GPRS + 0xd8)]
	ldr x29, [x4, #RPCS3_DUMP_FP]

	ldr x6, [x4, #RPCS3_SLOT_INDEX]
	mov w5, #1
	lsl w5, w5, w6
	mvn w5, w5
	adrp x2, s_slots_in_use
	add x2, x2, :lo12:s_slots_in_use
.Lrelease:
	ldaxr w3, [x2]
	and w3, w3, w5
	stlxr w7, w3, [x2]
	cbnz w7, .Lrelease

	mov w0, wzr
	svc #SVC_RETURN_FROM_EXCEPTION
	b .

.Lcrash:
	str x4, [x1, #0x00]
	ldr x6, [x4, #RPCS3_SLOT_INDEX]
	add x6, x6, #1
	adrp x2, s_slot_stacks
	add x2, x2, :lo12:s_slot_stacks
	add x2, x2, x6, lsl #RPCS3_EXCEPTION_STACK_SHIFT
	str x2, [x1, #RPCS3_FRAME_SP]
	adrp x2, .Lcrash_trampoline
	add x2, x2, :lo12:.Lcrash_trampoline
	str x2, [x1, #RPCS3_FRAME_PC]
	mov w0, wzr
	svc #SVC_RETURN_FROM_EXCEPTION
	b .

.Ldecline_locked:
	clrex
.Ldecline:
	mov w0, #RESULT_NOT_HANDLED
	svc #SVC_RETURN_FROM_EXCEPTION
	b .
.size __libnx_exception_entry, . - __libnx_exception_entry

.balign 16
.type .Lcrash_trampoline, %function
.Lcrash_trampoline:
	bl __libnx_exception_handler
	mov w0, wzr
	mov x1, xzr
	mov x2, xzr
	bl svcBreak
	b .

.section .bss.rpcs3_exception, "aw", %nobits
.balign 16
s_slots:
	.space RPCS3_EXCEPTION_SLOT_COUNT * RPCS3_EXCEPTION_SLOT_SIZE
s_slot_stacks:
	.space RPCS3_EXCEPTION_SLOT_COUNT * RPCS3_EXCEPTION_STACK_SIZE
.balign 4
s_slots_in_use:
	.space 4
