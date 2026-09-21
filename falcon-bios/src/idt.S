/* falcon-bios: the long-mode IDT (docs/notes/bios-route.md Section 5.3).
 *
 * WHY THERE IS ONE. Long mode with no IDT loaded does not ignore an
 * exception: the IDT register still describes the real-mode vector table
 * (base 0, limit 0x3FF, which is what the thunk loads on the way down), so
 * the processor reads a 16-byte gate out of the interrupt vector table, finds
 * garbage, raises a double fault the same way, and resets. A page fault, a
 * #GP from a bad pointer or a divide by zero anywhere in stage 2 or stage 3
 * was therefore an instant reboot with nothing on the screen and nothing on
 * serial.
 *
 * WHAT IT DOES. The 32 processor exceptions each land in a stub that makes
 * the stack the same shape for all of them (the processor pushes an error
 * code for some vectors and not others), and all of them go to bios_fault
 * (fault.c), which  says which exception, where, and then ends the way every
 * other failure does: on the screen, a key, a reset.
 *
 * It lives in stage 2 because stage 2 is resident and stage 3 is not
 * reachable from the thunk: the thunk (thunk.S) reloads the vector table on
 * the way down to real mode and this table on the way back up, and it can
 * only name a symbol it is linked with. Stage 3 inherits it untouched, since
 * nothing it does loads an IDT.
 *
 * Interrupts stay off in long mode, so only the exceptions have gates. An
 * NMI is vector 2 and is reported like the rest.
 */

#include "biosdefs.h"

/* The distance between two stubs, and so what fault_install steps by. */
#define FAULT_STUB 16

	.code64
	.section .text.stage2, "ax"
	.globl  fault_install
	.globl  idt_desc
	.extern bios_fault
	.extern bios_reboot

/* void fault_install(void): fills the table and loads it. */
fault_install:
	leaq    fault_stubs(%rip), %rax
	leaq    idt(%rip), %rdi
	movl    $32, %ecx
1:	movq    %rax, %rdx
	movw    %dx, (%rdi)          /* offset 15:0 */
	movw    $SEL_CODE64, 2(%rdi)
	movw    $0x8E00, 4(%rdi)     /* present, DPL 0, interrupt gate, IST 0 */
	shrq    $16, %rdx
	movw    %dx, 6(%rdi)         /* offset 31:16 */
	shrq    $16, %rdx
	movl    %edx, 8(%rdi)        /* offset 63:32 */
	movl    $0, 12(%rdi)
	addq    $FAULT_STUB, %rax
	addq    $16, %rdi
	loop    1b
	lidt    idt_desc(%rip)
	ret

/* One stub per vector, FAULT_STUB bytes apart so the table above is built
 * by adding rather than looked up. Each leaves an error code (the
 * processor's, or a zero) under the vector number. */
	.balign FAULT_STUB
fault_stubs:
	.set    vec, 0
	.rept   32
	.balign FAULT_STUB
	.if     (vec == 8) || ((vec >= 10) && (vec <= 14)) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
	pushq   $vec                 /* the processor pushed an error code */
	.else
	pushq   $0
	pushq   $vec
	.endif
	jmp     fault_common
	.set    vec, vec + 1
	.endr

/* The stack is vector, error code, then the processor's frame: RIP, CS,
 * RFLAGS, RSP, SS. Nothing is restored, because nothing returns.
 *
 * A second exception while the first is being reported (the report goes
 * through the thunk and the BIOS, which is where a damaged machine is most
 * likely to fault again) resets rather than recursing. */
fault_common:
	cld
	incb    fault_depth(%rip)
	cmpb    $1, fault_depth(%rip)
	jne     1f
	movq    (%rsp), %rdi         /* vector */
	movq    8(%rsp), %rsi        /* error code */
	movq    16(%rsp), %rdx       /* RIP */
	movq    %cr2, %rcx           /* the faulting address, for vector 14 */
	andq    $-16, %rsp           /* the ABI's alignment at a call */
	call    bios_fault
1:	andq    $-16, %rsp
	call    bios_reboot

	.balign 8
idt_desc:
	.word   32 * 16 - 1
	.quad   idt

/* In the image rather than .bss, which stage 2 may not have: nothing clears
 * it (falcon-bios/build.mk). */
	.balign 16
idt:
	.space  32 * 16
fault_depth:
	.byte   0
