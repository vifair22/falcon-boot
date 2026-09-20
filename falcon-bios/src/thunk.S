/* falcon-bios: the real-mode thunk (docs/notes/bios-route.md Section 5.3,
 * job 3 of the five, and Section 5.1's "200 lines nastier" that buying one
 * core build was worth paying).
 *
 * Calls a BIOS interrupt from 64-bit long mode and comes back. Everything the
 * loader wants from the firmware is behind one of these: INT 13h for disk,
 * INT 10h for the screen, INT 16h for the keyboard. falcon-core's read
 * callback is a thin C wrapper over the first.
 *
 * WHY THIS EXISTS AT ALL. The BIOS is 16-bit real-mode code. Long mode cannot
 * call it, and there is no firmware-independent way to ask a PC BIOS for a
 * disk sector. So the processor goes all the way down and all the way back
 * up, per call. It is the price of Section 5.1's decision to have one
 * falcon-core build rather than a 32-bit flavour, and it is paid here once.
 *
 * THE DESCENT, and every step is required:
 *
 *   long mode -> a 32-bit code segment, because paging cannot be turned off
 *                from a 64-bit one
 *             -> paging off, which clears EFER.LMA and leaves long mode
 *             -> EFER.LME off, so nothing tries to re-enter it
 *             -> a 16-bit code segment, so the limits cached in the segment
 *                registers are the 64 KB real mode expects
 *             -> CR0.PE off
 *             -> a far jump with a real-mode segment, and the IVT restored
 *
 * and the ascent is the same in reverse. What survives the round trip is the
 * register block below and nothing else: no stack, no pointer, nothing that
 * assumes an address above a megabyte.
 *
 * THE REGISTER BLOCK IS THE INTERFACE. The caller fills it, calls
 * thunk_bios with the vector in `thunk_vector`, and reads the results back
 * out of the same block. It lives in low memory because real mode has to be
 * able to reach it.
 */

#include "biosdefs.h"

	.extern gdt_desc

	.section .text.stage2, "ax"
	.code64
	.globl  thunk_bios
	.globl  thunk_vector
	.globl  thunk_regs

/* void thunk_bios(void): issues INT thunk_vector with the registers in
 * thunk_regs, and writes the returned registers back into it. */
thunk_bios:
	pushq   %rbx
	pushq   %rbp
	pushq   %r12
	pushq   %r13
	pushq   %r14
	pushq   %r15
	pushfq

	/* The 64-bit stack pointer cannot survive in a 16-bit world, so it is
	 * parked at a fixed low address and picked up on the way back. */
	movq    %rsp, saved_rsp(%rip)

	/* Patch the vector into the int instruction in the real-mode body
	 * below. A table of 256 stubs would be the alternative; this is one
	 * byte and the code is identity mapped and writable. */
	movzbl  thunk_vector(%rip), %eax
	movb    %al, real_int_op + 1(%rip)

	ljmp    *thunk_to32(%rip)

	.code32
thunk_32:
	movw    $SEL_DATA16, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* Paging off. This is the instruction that leaves long mode: with
	 * CR0.PG clear the processor clears EFER.LMA by itself. */
	movl    %cr0, %eax
	andl    $0x7FFFFFFF, %eax
	movl    %eax, %cr0

	/* CR3 is reloaded on the way back up, so nothing needs saving, but
	 * LME has to go or the next CR0.PG would re-enter long mode at a
	 * moment of the processor's choosing rather than ours. */
	movl    $0xC0000080, %ecx
	rdmsr
	andl    $0xFFFFFEFF, %eax    /* EFER.LME off */
	wrmsr

	ljmp    $SEL_CODE16, $thunk_16

	.code16
thunk_16:
	/* Protection off. The segment registers keep the 64 KB limits loaded
	 * from the 16-bit descriptors above, which is what makes the next far
	 * jump land in a working real mode. */
	movl    %cr0, %eax
	andl    $0xFFFFFFFE, %eax
	movl    %eax, %cr0

	ljmp    $0, $thunk_real

thunk_real:
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs
	movw    $0x7000, %sp         /* below stage 2, above nothing we use */

	/* The interrupt vector table, which long mode replaced with an IDT.
	 * Base 0, limit 0x3FF: the shape the BIOS put there at power on. */
	lidt    ivt_desc

	/* The caller's registers. DS is loaded last because everything up to
	 * here addresses through it. */
	movw    $thunk_regs, %si
	movl    0(%si), %eax
	movl    4(%si), %ebx
	movl    8(%si), %ecx
	movl    12(%si), %edx
	movl    20(%si), %edi
	movw    24(%si), %bp         /* es for the call */
	movl    16(%si), %esi
	pushw   %bp
	popw    %es

	sti
real_int_op:
	int     $0x00                /* the vector is patched in above */
	cli

	/* Back out, into the same block. SI is the only register that can
	 * hold the block's address and it is also one of the results, so the
	 * results go out through a scratch slot first. */
	pushfw
	pushw   %si
	movw    $thunk_regs, %si
	movl    %eax, 0(%si)
	movl    %ebx, 4(%si)
	movl    %ecx, 8(%si)
	movl    %edx, 12(%si)
	movl    %edi, 20(%si)
	popw    %ax
	movw    %ax, 16(%si)         /* the caller's si, as returned */
	popw    %ax
	movw    %ax, 28(%si)         /* flags, for the carry bit */

	/* ---- back up ---- */
	cli
	lgdt    gdt_desc

	movl    %cr0, %eax
	orl     $1, %eax             /* CR0.PE */
	movl    %eax, %cr0

	ljmpl   $SEL_CODE32, $thunk_back32

	.code32
thunk_back32:
	movw    $SEL_DATA64, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	movl    $PML4_ADDR, %eax
	movl    %eax, %cr3

	movl    $0xC0000080, %ecx
	rdmsr
	orl     $(1 << 8), %eax      /* EFER.LME */
	wrmsr

	movl    %cr0, %eax
	orl     $(1 << 31), %eax     /* CR0.PG, and LMA comes back with it */
	movl    %eax, %cr0

	ljmp    $SEL_CODE64, $thunk_back64

	.code64
thunk_back64:
	movw    $SEL_DATA64, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	movq    saved_rsp(%rip), %rsp

	popfq
	popq    %r15
	popq    %r14
	popq    %r13
	popq    %r12
	popq    %rbp
	popq    %rbx
	ret

	.balign 8
/* The far pointer the first jump takes: offset then selector. */
thunk_to32:
	.long   thunk_32
	.word   SEL_CODE32

	.balign 4
/* Real mode's interrupt vector table, as the BIOS left it. */
ivt_desc:
	.word   0x3FF
	.long   0

	.balign 8
saved_rsp:
	.quad   0

	.globl  thunk_vector
thunk_vector:
	.byte   0

	/* eax ebx ecx edx esi edi es flags, in that order, 32 bits each
	 * except es and flags which are 16 and padded. The caller writes what
	 * the BIOS call wants and reads what it returned out of the same
	 * place. */
	.balign 4
thunk_regs:
	.long   0    /*  0: eax */
	.long   0    /*  4: ebx */
	.long   0    /*  8: ecx */
	.long   0    /* 12: edx */
	.long   0    /* 16: esi */
	.long   0    /* 20: edi */
	.word   0    /* 24: es */
	.word   0    /* 26: padding */
	.word   0    /* 28: flags */
	.word   0    /* 30: padding */
