/* falcon-bios: the real-mode thunk.
 *
 * Calls a BIOS interrupt from 32-bit protected mode and comes back.
 * Everything the loader wants from the firmware is behind one of these:
 * INT 13h for disk, INT 10h for the screen's mode and cursor, INT 16h for the
 * keyboard, INT 15h for the memory map. falcon-core's read callback is a thin
 * C wrapper over the first.
 *
 * WHY THIS EXISTS AT ALL. The BIOS is 16-bit real-mode code. Protected mode
 * cannot call it, and there is no firmware-independent way to ask a PC BIOS
 * for a disk sector. So the processor goes down and back up, per call.
 *
 * THE DESCENT, and every step is required:
 *
 *   protected mode -> a 16-bit code segment, so the limits cached in the
 *                     segment registers are the 64 KB real mode expects
 *                  -> CR0.PE off
 *                  -> a far jump with a real-mode segment, and the IVT
 *                     restored
 *
 * and the ascent is the same in reverse.
 *
 * THE REGISTER BLOCK IS THE INTERFACE. The caller fills it, calls
 * thunk_bios with the vector in `thunk_vector`, and reads the results back
 * out of the same block. It lives in low memory because real mode has to be
 * able to reach it.
 */

#include <start/biosdefs.h>

	.extern gdt_desc
	.extern idt_desc

	.section .text.stage2, "ax"

/* A debug mark that preserves everything. Writing one from the middle of a
 * mode switch is how the worst bugs in this file were found, and the reason
 * it saves and restores rather than using a scratch register is that two of
 * those bugs WERE a marker clobbering one. */
.macro MARK32 ch
	pushfl
	pushl   %eax
	pushl   %edx
	movw    $0x3FD, %dx
9903:	inb     %dx, %al
	testb   $0x20, %al
	jz      9903b
	movw    $0x3F8, %dx
	movb    $\ch, %al
	outb    %al, %dx
	popl    %edx
	popl    %eax
	popfl
.endm

.macro MARK16 ch
	pushfw
	pushw   %ax
	pushw   %dx
	movw    $0x3FD, %dx
9902:	inb     %dx, %al
	testb   $0x20, %al
	jz      9902b
	movw    $0x3F8, %dx
	movb    $\ch, %al
	outb    %al, %dx
	popw    %dx
	popw    %ax
	popfw
.endm

	.code32
	.globl  thunk_bios

/* void thunk_bios(void): issues INT thunk_vector with the registers in
 * thunk_regs, and writes the returned registers back into it. Preserves
 * what the i386 System V ABI says a callee preserves. */
thunk_bios:
	pushl   %ebx
	pushl   %esi
	pushl   %edi
	pushl   %ebp
	pushfl

	/* The 32-bit stack pointer cannot survive in a 16-bit world, so it is
	 * parked at a fixed low address and picked up on the way back. */
	movl    %esp, saved_esp

	/* Patch the vector into the int instruction in the real-mode body
	 * below. A table of 256 stubs would be the alternative; this is one
	 * byte, and with paging off nothing makes the code read-only. */
	movzbl  thunk_vector, %eax
	movb    %al, real_int_op + 1

	cli
	ljmp    $SEL_CODE16, $thunk_16

	.code16
thunk_16:
	movw    $SEL_DATA16, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* Protection off. The segment registers keep the 64 KB limits just
	 * loaded from the 16-bit descriptors, which is what makes the next
	 * far jump land in a working real mode. */
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

	/* The interrupt vector table, which protected mode replaced with an
	 * IDT. Base 0, limit 0x3FF: the shape the BIOS put there at power on. */
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
	movw    %ax, 28(%si)         /* flags: carry, and zero for INT 16h */

	/* ---- back up ---- */
	cli
	lgdt    gdt_desc

	movl    %cr0, %eax
	orl     $1, %eax             /* CR0.PE */
	movl    %eax, %cr0

	ljmpl   $SEL_CODE32, $thunk_back32

	.code32
thunk_back32:
	movw    $SEL_DATA32, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	movl    saved_esp, %esp

	/* The IDT back, over the vector table loaded on the way down; without
	 * it an exception after the first BIOS call would read a gate out of
	 * the vector table and reset the machine (idt.S). */
	lidt    idt_desc

	popfl
	popl    %ebp
	popl    %edi
	popl    %esi
	popl    %ebx
	ret

	.balign 4
/* Real mode's interrupt vector table, as the BIOS left it. */
ivt_desc:
	.word   0x3FF
	.long   0

	.balign 4
saved_esp:
	.long   0

/* ---- the descent that does not come back ----
 *
 * Chainloading means handing the machine to a sixteen-bit program: an ISO's
 * own bootloader, read off the medium and placed where the El Torito record
 * says. Everything above is done at that point, so this is the thunk's
 * descent with the ascent removed and a far jump in place of the interrupt.
 *
 * It is separate from thunk_bios rather than a flag on it because the two
 * differ in what they are for, not in how they work: one borrows the
 * processor and gives it back, and this one gives it away. A shared routine
 * with a "come back?" argument would put the most delicate code in the tree
 * behind a branch.
 *
 * Reached through the gate at GATE_BOOT16. GATE_TARGET holds where to land,
 * offset then segment, and GATE_DRIVE the number to hand over in DL, which
 * is how the program about to run knows which drive it came off. GATE_SI is
 * SI, with DS zero: a partition's boot sector started the way a DOS or
 * Windows MBR starts it reads its own partition table entry at DS:SI
 * (mbr/chain.c puts one at 0000:07BE and up). Zero for every other route.
 */
	.code32
	.globl  boot16
boot16:
	cli
	ljmp    $SEL_CODE16, $boot16_16

	.code16
boot16_16:
	movw    $SEL_DATA16, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	movl    %cr0, %eax
	andl    $0xFFFFFFFE, %eax    /* CR0.PE */
	movl    %eax, %cr0

	ljmp    $0, $boot16_real

boot16_real:
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs
	movw    $0x7000, %sp

	lidt    ivt_desc

	/* DL is the contract: a bootloader reads the drive it was started
	 * from out of it and has no other way to know. */
	movb    GATE_DRIVE, %dl
	movw    GATE_SI, %si
	sti
	ljmp    *GATE_TARGET
