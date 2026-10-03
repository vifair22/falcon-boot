/* falcon-bios: where the other processors start for an unlock (mp/mp.c).
 *
 * A startup IPI starts a processor in real mode at the first byte of a
 * page below a megabyte, CS the page's number times 256 and IP 0, with its
 * cache off: INIT sets CR0.CD and CR0.NW, and a processor left that way
 * runs the derivation from memory, uncached. This page is copied to
 * APT_ADDR before the IPIs go out; everything here addresses it as
 * APT_ADDR and offsets into it, never by a link address, since it runs
 * somewhere other than where it was linked.
 *
 * In order: interrupts off; the cache on; the boot processor's GDT, which
 * mp.c copied into the page (sgdt); protected mode; the flat data
 * segments; this page's IDT, whose every gate is the halt below, so a
 * fault on one of these processors stops that processor rather than
 * triple faulting it, which on many chipsets resets the machine; the SSE
 * state as pmode.S sets it, for the vector block functions; a stack, by a
 * ticket taken with lock xadd, the processors past the last stack going
 * straight to the halt; then mp_ap_main(ticket). It returns here, and the
 * processor halts with interrupts off until mp.c sends it INIT, which is
 * the waiting state the BIOS left it in.
 */

#include <start/biosdefs.h>

	.section .rodata.aptramp, "a"
	.globl  aptramp_start

	.code16
aptramp_start:
	jmp     ap16

	.org    APT_OFF_GDTR
	.space  6
	.org    APT_OFF_IDTR
	.space  6
	.org    APT_OFF_ENTRY
	.long   0
	.org    APT_OFF_STACKS
	.long   0
	.org    APT_OFF_STACKB
	.long   0
	.org    APT_OFF_NSTACK
	.long   0
	.org    APT_OFF_TICKET
	.long   0
	.org    APT_OFF_IDT
	.space  APT_IDT_GATES * 8

	.org    APT_OFF_HALT
	.code32
ap_halt:
	cli
	hlt
	jmp     ap_halt

	.org    APT_OFF_CODE
	.code16
ap16:
	cli
	cld
	movw    %cs, %ax
	movw    %ax, %ds
	movl    %cr0, %eax
	andl    $0x9FFFFFFF, %eax    /* CD and NW clear: the cache on */
	movl    %eax, %cr0
	lgdtl   APT_OFF_GDTR
	movl    %cr0, %eax
	orl     $1, %eax             /* CR0.PE */
	movl    %eax, %cr0
	ljmpl   $SEL_CODE32, $(APT_ADDR + ap32 - aptramp_start)

	.code32
ap32:
	movw    $SEL_DATA32, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs
	lidt    APT_ADDR + APT_OFF_IDTR

	/* The SSE state, when there is SSE: pmode.S's rule, per processor. */
	movl    $1, %eax
	cpuid
	andl    $((1 << 24) | (1 << 25)), %edx   /* FXSR, SSE */
	cmpl    $((1 << 24) | (1 << 25)), %edx
	jne     1f
	movl    %cr4, %eax
	orl     $((1 << 9) | (1 << 10)), %eax    /* OSFXSR, OSXMMEXCPT */
	movl    %eax, %cr4
1:
	movl    $1, %eax
	lock xaddl %eax, APT_ADDR + APT_OFF_TICKET
	cmpl    APT_ADDR + APT_OFF_NSTACK, %eax
	jae     2f
	leal    1(%eax), %ecx
	imull   APT_ADDR + APT_OFF_STACKB, %ecx
	addl    APT_ADDR + APT_OFF_STACKS, %ecx
	movl    %ecx, %esp
	subl    $12, %esp            /* 16-byte aligned at the call */
	pushl   %eax
	call    *APT_ADDR + APT_OFF_ENTRY
2:
	jmp     ap_halt              /* relative: right wherever the page runs */
	/* The page is APT_BYTES exactly, which is what lay_page copies: past
	 * it this .org would move backwards, which gas refuses. */
	.org    APT_BYTES
