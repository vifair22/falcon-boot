/* falcon jump: the reverse thunk, a module's INT 13h entry from real mode
 * (docs/DESIGN.md Section 14.6).
 *
 * WHY THIS EXISTS. A bootloader started off a CD or a USB stick reads its
 * medium through INT 13h in real mode, and the driver that can answer is a
 * module: protected-mode C at its slot above a megabyte. Stage 2's thunk goes
 * the other way (C calls down into the BIOS); this is the way back up, the
 * same pair of mode switches MEMDISK and Plop make. It answers one drive and
 * hands every other call to the handler it replaced.
 *
 * WHAT IT DECIDES: nothing. Which call it is and what the answer is are the
 * module's (jmod.h, jmod_ops.int13); this saves the caller, switches mode,
 * calls, switches back and restores. The module's resident half may use
 * nothing of the core, which is gone by the time a bootloader calls this.
 *
 * WHAT IT HAS TO PRESERVE, and each has bitten a loader of this shape:
 *   - the caller's GDTR. A loader that thunks down into the BIOS itself (a
 *     falcon-bios stage 3 on the stick, when jump started it) returns to
 *     protected mode through the table it loaded; this loads its own and
 *     puts the caller's back.
 *   - A20. A DOS-era caller may have it off, and the module's slot is above
 *     a megabyte, where a masked A20 reads the wrong byte. Port 0x92's bit 1
 *     goes on for the call and back to what it was.
 *   - a caller in virtual 8086 mode, which cannot be taken into protected
 *     mode from under its monitor: the call fails with AH=01h instead.
 *   - interrupts, which stay off throughout: the IDT register still describes
 *     the real-mode vector table, so an interrupt taken in protected mode
 *     would read it as descriptors. The module polls.
 *
 * POSITION INDEPENDENCE, as vcdres.S: the block is copied to wherever the
 * BIOS data area's count leaves room, so real-mode code reaches its data as
 * CS-relative offsets from biosdefs.h placed with .org, and the 32-bit code
 * finds the block's linear address from where it was called. The installer
 * (jres.c) patches in the segment, the linear address and the GDT's base.
 */

#include <start/biosdefs.h>

#define REG_EAX   (JRES_OFF_REGS + 0)
#define REG_EBX   (JRES_OFF_REGS + 4)
#define REG_ECX   (JRES_OFF_REGS + 8)
#define REG_EDX   (JRES_OFF_REGS + 12)
#define REG_ESI   (JRES_OFF_REGS + 16)
#define REG_EDI   (JRES_OFF_REGS + 20)
#define REG_DS    (JRES_OFF_REGS + 24)
#define REG_ES    (JRES_OFF_REGS + 26)
#define REG_FLAGS (JRES_OFF_REGS + 28)

#define SEL_C32 0x08
#define SEL_D32 0x10
#define SEL_C16 0x18
#define SEL_D16 0x20

	.section .text.jres, "ax"
	.globl  jres_blob_start
	.globl  jres_blob_end

	.code16
jres_blob_start:
	.org    JRES_OFF_OLDINT13
	.long   0
	.org    JRES_OFF_DRIVE
	.byte   0
	.org    JRES_OFF_A20
	.byte   0
	.org    JRES_OFF_GDTR
	.word   5 * 8 - 1
	.long   0                    /* the GDT's linear address, installed */
	.org    JRES_OFF_SAVEGDT
	.word   0
	.long   0
	.org    JRES_OFF_SAVESP
	.word   0
	.org    JRES_OFF_SAVESS
	.word   0
	.org    JRES_OFF_ENTRY
	.long   0
	.org    JRES_OFF_STACK
	.long   0
	.org    JRES_OFF_PM32PTR
	.long   jres_pm32 - jres_blob_start   /* the block's address is added */
	.word   SEL_C32
	.org    JRES_OFF_RMPTR
	.word   jres_rm16 - jres_blob_start
	.word   0                    /* the block's segment, installed */
	.org    JRES_OFF_PM16PTR
	.long   jres_pm16 - jres_blob_start   /* relative to SEL_C16's base */
	.word   SEL_C16
	.org    JRES_OFF_NE820
	.word   0
	.org    JRES_OFF_E801LOW
	.word   0xFFFF
	.org    JRES_OFF_E801CAP
	.word   0xFFFF

	.org    JRES_OFF_GDT
	.quad   0
	.quad   0x00CF9A000000FFFF   /* 0x08: code32, flat */
	.quad   0x00CF92000000FFFF   /* 0x10: data32, flat */
	.quad   0x00009A000000FFFF   /* 0x18: code16, base the block, installed */
	.quad   0x000092000000FFFF   /* 0x20: data16, base 0, real mode's limits */
	.org    JRES_OFF_OLDINT15
	.long   0

	.org    JRES_OFF_REGS
	.fill   32, 1, 0

	.org    JRES_OFF_CODE
jres_int13:
	cmpb    %cs:JRES_OFF_DRIVE, %dl
	je      1f
	ljmp    *%cs:JRES_OFF_OLDINT13
	/* A caller in virtual 8086 mode runs with CR0.PE set under a monitor
	 * (EMM386, Windows 9x); the CR0 write below would fault to it. SMSW is
	 * not privileged, so the check costs nothing, and the call fails with
	 * AH=01h instead. */
1:	pushw   %ax
	smsw    %ax
	testb   $1, %al
	popw    %ax
	jz      2f
	pushw   %bp
	movw    %sp, %bp
	orw     $1, 6(%bp)           /* CF in the iret frame */
	popw    %bp
	movb    $0x01, %ah
	iret
2:	cli
	movl    %eax, %cs:REG_EAX
	movl    %ebx, %cs:REG_EBX
	movl    %ecx, %cs:REG_ECX
	movl    %edx, %cs:REG_EDX
	movl    %esi, %cs:REG_ESI
	movl    %edi, %cs:REG_EDI
	movw    %ds, %cs:REG_DS
	movw    %es, %cs:REG_ES
	movw    $0, %cs:REG_FLAGS
	movw    %sp, %cs:JRES_OFF_SAVESP
	movw    %ss, %cs:JRES_OFF_SAVESS
	sgdtl   %cs:JRES_OFF_SAVEGDT

	/* A20 on, bit 0 left alone: writing it resets the machine. */
	inb     $0x92, %al
	movb    %al, %cs:JRES_OFF_A20
	orb     $0x02, %al
	andb    $0xFE, %al
	outb    %al, $0x92

	lgdtl   %cs:JRES_OFF_GDTR
	movl    %cr0, %eax
	orb     $1, %al
	movl    %eax, %cr0
	ljmpl   *%cs:JRES_OFF_PM32PTR

	.code32
jres_pm32:
	movw    $SEL_D32, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	/* Where this block is: where this code is, less its offset in it. */
	call    2f
2:	popl    %ebx
	subl    $(2b - jres_blob_start), %ebx
	movl    JRES_OFF_STACK(%ebx), %esp
	leal    JRES_OFF_REGS(%ebx), %eax
	pushl   %eax
	call    *JRES_OFF_ENTRY(%ebx)
	/* ebx is callee-saved; it is still the block. */
	ljmpl   *JRES_OFF_PM16PTR(%ebx)

	.code16
jres_pm16:
	/* 16-bit protected mode, CS based at the block: the data selectors get
	 * real mode's 64 KB limits before protection goes off, because real mode
	 * keeps using whatever limit is cached. */
	movw    $SEL_D16, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movl    %cr0, %eax
	andb    $0xFE, %al
	movl    %eax, %cr0
	ljmp    *%cs:JRES_OFF_RMPTR

jres_rm16:
	lgdtl   %cs:JRES_OFF_SAVEGDT
	movb    %cs:JRES_OFF_A20, %al
	andb    $0xFE, %al
	outb    %al, $0x92
	movw    %cs:JRES_OFF_SAVESS, %ss
	movw    %cs:JRES_OFF_SAVESP, %sp
	movl    %cs:REG_EAX, %eax
	movl    %cs:REG_EBX, %ebx
	movl    %cs:REG_ECX, %ecx
	movl    %cs:REG_EDX, %edx
	movl    %cs:REG_ESI, %esi
	movl    %cs:REG_EDI, %edi
	movw    %cs:REG_DS, %ds
	movw    %cs:REG_ES, %es
	/* The carry flag, into the flags the iret restores. */
	pushw   %bp
	movw    %sp, %bp
	testb   $1, %cs:REG_FLAGS
	jz      3f
	orw     $1, 6(%bp)
	jmp     4f
3:	andw    $0xFFFE, 6(%bp)
4:	popw    %bp
	iret

/* ---- INT 15h: the memory map, for the USB route (DESIGN Section 14.6) ----
 *
 * vcdres.S's INT 15h half, for a route with no virtual CD to carry one: the
 * E820 map jres.c carved (e820_carve, host-tested), E801's two counts held
 * to their ceilings, and AH=88h to the lower one. It decides nothing; the
 * arithmetic is the installer's. Every other call goes to the handler it
 * replaced, and the flags come back from that handler's iret into ours.
 */
	.org    JRES_OFF_CODE15
jres_int15:
	cmpw    $0xE801, %ax
	je      e801
	cmpb    $0x88, %ah
	je      ah88
	cmpl    $0x0000E820, %eax
	jne     5f
	cmpl    $0x534D4150, %edx    /* "SMAP" */
	jne     5f
	cmpw    $0, %cs:JRES_OFF_NE820
	jne     e820_next
5:	ljmp    *%cs:JRES_OFF_OLDINT15

/* EBX is the index wanted, 0 to start; it comes back as the next one, or 0
 * after the last. */
e820_next:
	cmpl    $20, %ecx
	jb      e820_bad
	movzwl  %cs:JRES_OFF_NE820, %eax
	cmpl    %eax, %ebx
	jae     e820_bad
	pushw   %ds
	pushl   %esi
	pushw   %di
	imull   $20, %ebx, %esi
	addw    $JRES_OFF_E820, %si
	pushw   %cs
	popw    %ds
	movw    $10, %cx
	cld
	rep movsw
	popw    %di
	popl    %esi
	popw    %ds
	incl    %ebx
	cmpl    %eax, %ebx
	jb      6f
	xorl    %ebx, %ebx
6:	movl    $0x534D4150, %eax
	movl    $20, %ecx
	jmp     ok15
e820_bad:
	movb    $0x86, %ah
	jmp     fail15

ah88:
	pushfw
	lcall   *%cs:JRES_OFF_OLDINT15
	jc      fail15
	cmpw    %cs:JRES_OFF_E801LOW, %ax
	jbe     ok15
	movw    %cs:JRES_OFF_E801LOW, %ax
	jmp     ok15

e801:
	pushfw
	lcall   *%cs:JRES_OFF_OLDINT15
	jc      fail15
	cmpw    %cs:JRES_OFF_E801CAP, %bx
	jbe     7f
	movw    %cs:JRES_OFF_E801CAP, %bx
7:	cmpw    %cs:JRES_OFF_E801CAP, %dx
	jbe     8f
	movw    %cs:JRES_OFF_E801CAP, %dx
8:	cmpw    %cs:JRES_OFF_E801LOW, %ax
	jbe     9f
	movw    %cs:JRES_OFF_E801LOW, %ax
9:	cmpw    %cs:JRES_OFF_E801LOW, %cx
	jbe     ok15
	movw    %cs:JRES_OFF_E801LOW, %cx

ok15:
	pushw   %bp
	movw    %sp, %bp
	andw    $0xFFFE, 6(%bp)
	popw    %bp
	iret
fail15:
	pushw   %bp
	movw    %sp, %bp
	orw     $0x0001, 6(%bp)
	popw    %bp
	iret

	.org    JRES_OFF_E820
	.space  (JRES_E820_MAX * 20)

	.org    JRES_BYTES - 1
	.byte   0
jres_blob_end:
