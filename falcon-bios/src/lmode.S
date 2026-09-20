/* falcon-bios: real mode to long mode (docs/notes/bios-route.md Section 5.3,
 * job 2 of the five the assembly is allowed to do).
 *
 * Entered in 16-bit real mode, returns never: it far-jumps into 64-bit code
 * at the address the caller put in `lmode_entry`. Everything after that jump
 * is C's, which is the whole point of doing this early.
 *
 * WHY LONG MODE AND NOT 32-BIT PROTECTED MODE. Section 5.1 measured it:
 * falcon-core contains no `unsigned long`, no `uintptr_t`, no
 * `sizeof(void *)` and no pointer-to-integer casts, so a 32-bit flavour would
 * work. It is still the wrong call, because a second flavour doubles the
 * build matrix, the test matrix and the coverage gate permanently, in
 * exchange for avoiding a long-to-real thunk perhaps 200 lines nastier than a
 * protected-to-real one. Pay the 200 lines once. One core build, and AES-NI
 * stays.
 *
 * THE ORDER BELOW IS NOT NEGOTIABLE and each step says what breaks without
 * it. Getting it wrong does not produce an error; it produces a triple fault,
 * which on real hardware is an instant reboot with nothing on the screen.
 */

#include "biosdefs.h"

	.code16
	.section .text.stage2, "ax"
	.globl  lmode_enter
	.globl  lmode_entry
	.extern puts_rm

/* Far-jumps to *lmode_entry in 64-bit mode. Does not return. */
lmode_enter:
	cli

	/* ---- 1. A20 ----
	 *
	 * Without it every odd megabyte of physical address space wraps to the
	 * one below, which would make the page tables below read as something
	 * other than what was written. The fast gate (port 0x92 bit 1) is what
	 * every machine since the mid-1990s has; the BIOS call is tried first
	 * because a few chipsets take offence at the port write, and the
	 * result is verified afterwards rather than assumed. */
	movw    $0x2401, %ax
	int     $0x15
	/* The BIOS call re-enables interrupts. SeaBIOS's INT 15h handler ends
	 * with sti whatever the caller's flags were, and the cli above is
	 * therefore undone by the very next instruction unless it is repeated
	 * here. Measured: without this the zeroing loop below is interrupted
	 * by a timer tick, and if A20 has not in fact opened, that loop has
	 * just overwritten the interrupt vector table it dispatches through.
	 * The machine then wanders into the BIOS ROM, which is exactly what
	 * `info registers` showed: CS=f000 with CR0 still 0x10. */
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
	 * low one changed. A machine that reaches here with A20 still closed
	 * cannot run anything this loader does next, so it says so and stops
	 * rather than corrupting the first megabyte and wandering. */
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
	call    puts_rm
a20_dead:
	hlt
	jmp     a20_dead
a20_ok:

	/* ---- 2. The page tables ----
	 *
	 * Built now, in real mode, because after CR0.PG there is no way to
	 * fix a mistake. Three pages, zeroed first: a stale entry left by the
	 * BIOS would be honoured by the MMU as readily as one we wrote.
	 *
	 * PML4[0] -> PDPT, PDPT[0] -> PD, and PD's 512 entries are 2 MB pages
	 * covering the first gigabyte, identity mapped. Present|Writable on
	 * every level, plus PS on the 2 MB entries, which is the bit that
	 * stops the walk at the page directory. */
	/* THE TABLES ARE REACHED THROUGH A SEGMENT, NOT A FLAT OFFSET.
	 *
	 * A real-mode segment is 64 KB and the processor enforces it. Writing
	 * to 0x10000 as `mov %eax,(%edi)` with EDI = 0x10000 and DS = 0 is an
	 * offset past the segment limit, which raises #GP, which dispatches
	 * through the interrupt vector table into the BIOS. Measured before
	 * this comment existed: `info registers` showed CS=f000 with CR0 still
	 * 0x10, in the middle of what looked like a perfectly good loop.
	 *
	 * ES = 0x1000 puts linear 0x10000 at offset 0, and all three tables
	 * are 12 KB, so one segment covers the lot: PML4 at ES:0x0000, PDPT at
	 * ES:0x1000, PD at ES:0x2000. Every offset below is 16-bit and inside
	 * the limit. */
	movl    $PML4_ADDR, %eax
	movl    %eax, %cr3           /* CR3 is linear; only the writes are segmented */
	movw    $(PML4_ADDR >> 4), %ax
	movw    %ax, %es

	xorw    %di, %di
	xorl    %eax, %eax
	movw    $(3 * 4096 / 4), %cx
3:	movl    %eax, %es:(%di)
	addw    $4, %di
	decw    %cx
	jnz     3b

	movl    $(PDPT_ADDR | 0x03), %es:(0x0000)   /* PML4[0] -> PDPT */
	movl    $(PD_ADDR | 0x03), %es:(0x1000)     /* PDPT[0] -> PD */

	movw    $0x2000, %di                        /* the page directory */
	xorl    %eax, %eax
	movw    $(IDENTITY_MAP / PAGE_2M), %cx
2:	movl    %eax, %edx
	orl     $0x83, %edx          /* Present | Writable | PageSize */
	movl    %edx, %es:(%di)
	movl    $0, %es:4(%di)       /* the high half of the entry */
	addl    $PAGE_2M, %eax
	addw    $8, %di
	loop    2b

	/* ---- 3. PAE ----
	 *
	 * Long mode has no non-PAE paging. Setting LME below without this is
	 * one of the ways to triple fault at the jump rather than at the
	 * instruction that was actually wrong. */
	movl    %cr4, %eax
	orl     $(1 << 5), %eax      /* CR4.PAE */
	movl    %eax, %cr4

	/* ---- 4. EFER.LME ----
	 *
	 * Arms long mode. It does not enter it: the processor is in
	 * "compatibility mode pending" until paging comes on, which is why
	 * the order here is LME then PG and not the other way round. */
	movl    $0xC0000080, %ecx    /* IA32_EFER */
	rdmsr
	orl     $(1 << 8), %eax      /* EFER.LME */
	wrmsr

	/* ---- 5. Protection and paging, together ----
	 *
	 * PE and PG in one write. Enabling PG while PE is clear is undefined,
	 * and enabling PE alone would drop us into 16-bit protected mode with
	 * a code segment that is not the one the GDT below describes. */
	lgdt    gdt_desc

	/* NOTHING MAY TOUCH EAX BETWEEN BUILDING THIS VALUE AND WRITING IT,
	 * and the same goes for the zeroing loop above.
	 *
	 * Both were broken by the same thing while this file was written: a
	 * single-character debug marker, `movb $'x', %al`, placed between
	 * setting a register up and using it. Here it turned 0x80000011 into
	 * 0x80000030, setting PG with PE clear, which is undefined. Above it
	 * turned the zero the tables are cleared with into 0x6e, so every
	 * entry named a physical address that does not exist and the first
	 * instruction fetch after paging came on took a page fault.
	 *
	 * Neither failure looked like its cause. The first showed as CR0 stuck
	 * at its reset value with CR4 and EFER correctly armed; the second as
	 * long mode genuinely active (EFER.LMA set) with CS never reloaded and
	 * CR2 holding the address of the very next instruction. Reading the
	 * page tables out of the monitor is what named it: the PML4 entry read
	 * 0x0000006e00011003, and 0x6e is the letter the marker printed. */
	movl    %cr0, %eax
	orl     $((1 << 31) | (1 << 0)), %eax   /* CR0.PG | CR0.PE */
	movl    %eax, %cr0

	/* ---- 6. The far jump that makes it real ----
	 *
	 * Until this executes, CS still describes a real-mode segment and the
	 * processor is in compatibility mode. The jump loads a 64-bit code
	 * descriptor and only then is this long mode. */
	ljmpl   $SEL_CODE64, $lmode_64

	.code64
lmode_64:
	/* Data segments are ignored for addressing in long mode but the
	 * registers still have to hold something valid, because a descriptor
	 * left over from real mode faults on the first push. */
	movw    $SEL_DATA64, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    %ax, %fs
	movw    %ax, %gs

	/* A stack below the boot sector, where nothing else lives. */
	movq    $0x7C00, %rsp

	movq    lmode_entry, %rax
	jmp     *%rax

	.code16

/* The GDT. Flat, and no more than long mode needs.
 *
 * The base and limit fields are ignored in 64-bit mode; what matters is the
 * L bit in the code descriptor, which is what distinguishes 64-bit code from
 * 32-bit compatibility code. A GDT whose code descriptor lacks it produces a
 * processor that has entered long mode and is executing 32-bit instructions,
 * which fails somewhere later and looks nothing like the cause. */
	.balign 8
gdt:
	.quad   0                    /* the mandatory null descriptor */
	/* code64: present, ring 0, code, executable, readable, L=1 */
	.quad   0x00AF9A000000FFFF
	/* data64: present, ring 0, data, writable */
	.quad   0x00CF92000000FFFF
gdt_end:

	.balign 4
gdt_desc:
	.word   gdt_end - gdt - 1
	.long   gdt

msg_a20:
	.asciz  "falcon-bios: A20 will not open; this machine cannot be booted\r\n"

	.balign 8
lmode_entry:
	.quad   0
