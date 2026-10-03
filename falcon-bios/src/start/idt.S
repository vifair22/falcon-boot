/* falcon-bios: the protected-mode IDT.
 *
 * WHY THERE IS ONE. Protected mode with no IDT loaded does not ignore an
 * exception: the IDT register still describes the real-mode vector table
 * (base 0, limit 0x3FF, which is what the thunk loads on the way down), so
 * the processor reads an 8-byte gate out of the interrupt vector table, finds
 * garbage, raises a double fault the same way, and resets. A #GP from a bad
 * pointer or a divide by zero anywhere in stage 2 or stage 3 was therefore an
 * instant reboot with nothing on the screen and nothing on serial.
 *
 * WHAT IT DOES. The 32 processor exceptions each land in a stub that makes
 * the stack the same shape for all of them (the processor pushes an error
 * code for some vectors and not others), and all of them go to bios_fault
 * (fault.c), which says which exception, where, and then ends the way every
 * other failure does: on the screen, a key, a reset.
 *
 * It lives in stage 2 because stage 2 is resident and stage 3 is not
 * reachable from the thunk: the thunk (thunk.S) reloads the vector table on
 * the way down to real mode and this table on the way back up, and it can
 * only name a symbol it is linked with. Stage 3 inherits it untouched, since
 * nothing it does loads an IDT.
 *
 * Interrupts stay off in protected mode, so only the exceptions have gates.
 * An NMI is vector 2 and is reported like the rest.
 */

#include <start/biosdefs.h>

/* The distance between two stubs, and so what fault_install steps by. */
#define FAULT_STUB 16

	.code32
	.section .text.stage2, "ax"
	.globl  fault_install
	.globl  idt_desc
	.extern bios_fault
	.extern bios_reboot

/* void fault_install(void): fills the table and loads it. */
fault_install:
	pushl   %edi
	movl    $fault_stubs, %eax
	movl    $idt, %edi
	movl    $32, %ecx
1:	movw    %ax, (%edi)          /* offset 15:0 */
	movw    $SEL_CODE32, 2(%edi)
	movw    $0x8E00, 4(%edi)     /* present, DPL 0, 32-bit interrupt gate */
	movl    %eax, %edx
	shrl    $16, %edx
	movw    %dx, 6(%edi)         /* offset 31:16 */
	addl    $FAULT_STUB, %eax
	addl    $8, %edi
	loop    1b
	lidt    idt_desc
	popl    %edi
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
	pushl   $vec                 /* the processor pushed an error code */
	.else
	pushl   $0
	pushl   $vec
	.endif
	jmp     fault_common
	.set    vec, vec + 1
	.endr

/* The stack is vector, error code, then the processor's frame: EIP, CS,
 * EFLAGS. Nothing is restored, because nothing returns.
 *
 * A second exception while the first is being reported (the report goes
 * through the thunk and the BIOS, which is where a damaged machine is most
 * likely to fault again) resets rather than recursing. */
fault_common:
	cld
	incb    fault_depth
	cmpb    $1, fault_depth
	jne     1f
	movl    (%esp), %eax         /* vector */
	movl    4(%esp), %ecx        /* error code */
	movl    8(%esp), %edx        /* EIP */
	andl    $-16, %esp           /* the ABI's alignment at a call */
	subl    $4, %esp
	pushl   %edx                 /* bios_fault(vec, err, eip) */
	pushl   %ecx
	pushl   %eax
	call    bios_fault
1:	andl    $-16, %esp
	call    bios_reboot

	.balign 4
idt_desc:
	.word   32 * 8 - 1
	.long   idt

/* In the image rather than .bss, which stage 2 may not have: nothing clears
 * it (falcon-bios/build.mk). */
	.balign 8
idt:
	.space  32 * 8
fault_depth:
	.byte   0
