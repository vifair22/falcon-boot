/* falcon-bios: real mode to 32-bit protected mode.
 *
 * Entered in 16-bit real mode, returns never: it far-jumps into 32-bit code
 * at the address the caller put in `pmode_entry`. Everything after that jump
 * is C's, which is the whole point of doing this early.
 *
 * WHY PROTECTED MODE AND NOT LONG MODE. The machines a BIOS route is for
 * include ones with no 64-bit mode, and a flat 32-bit environment runs on
 * every one of them and on every newer one too. What a newer processor adds
 * is taken by asking CPUID here and in falcon-core, not by building twice.
 * Nothing this loader places needs an address past 4 GiB, and paging stays
 * off, so a linear address is a physical one.
 *
 * THE ORDER BELOW IS NOT NEGOTIABLE and each step says what breaks without
 * it. Getting it wrong does not produce an error; it produces a triple fault,
 * which on real hardware is an instant reboot with nothing on the screen.
 */

#include <start/biosdefs.h>

	.code16
	.section .text.stage2, "ax"
	.globl  pmode_enter
	.globl  pmode_entry
	.extern fatal_rm

/* Far-jumps to *pmode_entry in 32-bit protected mode. Does not return. */
pmode_enter:
	cli

	/* ---- 1. A20 ----
	 *
	 * Without it every odd megabyte of physical address space wraps to the
	 * one below, and stage 3 at 1 MiB would land on the vector table. The
	 * fast gate (port 0x92 bit 1) is what every machine since the
	 * mid-1990s has; the BIOS call is tried first because a few chipsets
	 * take offence at the port write, and the result is verified
	 * afterwards rather than assumed. */
	movw    $0x2401, %ax
	int     $0x15
	/* The BIOS call re-enables interrupts. SeaBIOS's INT 15h handler ends
	 * with sti whatever the caller's flags were, and the cli above is
	 * therefore undone by the very next instruction unless it is repeated
	 * here. Without it a timer tick lands mid-way through writing low
	 * memory and the machine wanders into the BIOS ROM (CS=f000, CR0 still
	 * 0x10). */
	cli

	inb     $0x92, %al
	testb   $0x02, %al
	jnz     1f                   /* already open; writing again can lock */
	orb     $0x02, %al
	andb    $0xFE, %al           /* bit 0 is a fast reset: never set it */
	outb    %al, $0x92
1:
	/* Verified, not assumed. The test is the wraparound A20 exists to
	 * stop: write different values one megabyte apart and see whether the
	 * low one changed. */
	pushw   %ds
	pushw   %es
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    $0xFFFF, %ax
	movw    %ax, %es
	movw    $0x1234, (0x0500)
	movw    $0x5678, %es:(0x0510)   /* linear 0x100500 when A20 is open */
	movw    (0x0500), %ax
	popw    %es
	popw    %ds
	cmpw    $0x1234, %ax
	je      a20_ok
	movw    $msg_a20, %si
	jmp     fatal_rm
a20_ok:

	/* ---- 2. The SSE state, when there is SSE ----
	 *
	 * OSFXSR and OSXMMEXCPT are for one translation unit: falcon-core's
	 * AES-NI path, the only thing in this build compiled with SSE at all.
	 * Without them the first XMM instruction raises #UD. A processor
	 * without FXSR and SSE faults on the CR4 write itself, so the bits are
	 * set only when CPUID says both are there; that processor also has no
	 * AES-NI, and falcon-core's CPUID probe keeps it on the portable path.
	 * This is the whole of what "promote at runtime" means for the loader.
	 *
	 * stage2.S has already required CPUID (it asks for CMOV), so asking
	 * here cannot fault. */
	movl    $1, %eax
	cpuid
	andl    $((1 << 24) | (1 << 25)), %edx   /* FXSR, SSE */
	cmpl    $((1 << 24) | (1 << 25)), %edx
	jne     2f
	movl    %cr4, %eax
	orl     $((1 << 9) | (1 << 10)), %eax    /* OSFXSR, OSXMMEXCPT */
	movl    %eax, %cr4
2:
	/* ---- 3. Protection ----
	 *
	 * The GDT, then CR0.PE, then the far jump that loads a 32-bit code
	 * descriptor. Until the jump executes, CS still holds a real-mode
	 * segment. Nothing may touch EAX between reading CR0 and writing it:
	 * a debug marker placed there once set PG with PE clear. */
	lgdt    gdt_desc
	movl    %cr0, %eax
	orl     $1, %eax             /* CR0.PE */
	movl    %eax, %cr0
	ljmpl   $SEL_CODE32, $pmode_32

	.code32
pmode_32:
	/* Every data segment register is reloaded: a descriptor left over from
	 * real mode faults on the first push. */
	movw    $SEL_DATA32, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* A stack below the boot sector, where nothing else lives. */
	movl    $0x7C00, %esp

	jmp     *pmode_entry

	.code16

/* The GDT. Flat, and no more than the loader and the thunk need.
 *
 * code32 and data32 cover all 4 GiB, page granular, D=1. code16 and data16
 * have 64 KB limits, byte granular: real mode uses whatever limit is cached
 * in a segment register when PE is cleared, so these are what make the
 * thunk's return to real mode land with segments that behave. */
	.balign 8
gdt:
	.quad   0                    /* the mandatory null descriptor */
	.quad   0x00CF9A000000FFFF   /* code32: present, ring 0, exec/read */
	.quad   0x00CF92000000FFFF   /* data32: present, ring 0, read/write */
	.quad   0x008F9A000000FFFF   /* code16 */
	.quad   0x008F92000000FFFF   /* data16 */
gdt_end:

	/* The GDT is needed by the thunk too, so its descriptor is shared
	 * rather than duplicated. */
	.globl  gdt_desc

	.balign 4
gdt_desc:
	.word   gdt_end - gdt - 1
	.long   gdt

msg_a20:
	.asciz  "falcon-bios: A20 will not open; this machine cannot be booted\r\n"

	.balign 4
pmode_entry:
	.long   0
