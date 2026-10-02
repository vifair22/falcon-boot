/* falcon jump: the reverse thunk, a module's INT 13h and INT 16h entries
 * from real mode (docs/DESIGN.md Section 13.6).
 *
 * WHY THIS EXISTS. A bootloader started off a CD or a USB stick reads its
 * medium through INT 13h in real mode, and the driver that can answer is a
 * module: protected-mode C at its slot above a megabyte. Stage 2's thunk goes
 * the other way (C calls down into the BIOS); this is the way back up, the
 * same pair of mode switches MEMDISK and Plop make. It answers one drive and
 * hands every other call to the handler it replaced. On the USB route it
 * also stands in front of INT 16h, so the module's keyboards (hw/usb/hid.c)
 * are polled before the BIOS's own handler reads its ring.
 *
 * WHAT IT DECIDES: nothing. Which call it is and what the answer is are the
 * module's (jmod.h, jmod_ops.int13 and int16); this saves the caller,
 * switches mode, calls, switches back and restores. The module's resident half may use
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
 *     mode from under its monitor: an INT 13h call fails with AH=01h
 *     instead, and an INT 16h call goes to the BIOS unpolled.
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
	safe_mbr_hook JRES_OFF_OLDINT13
	cmpb    %cs:JRES_OFF_DRIVE, %dl
	je      1f
	ljmp    *%cs:JRES_OFF_OLDINT13
	/* A caller in virtual 8086 mode runs with CR0.PE set under a monitor
	 * (EMM386, Windows 9x); the CR0 write in jres_up would fault to it.
	 * SMSW is not privileged, so the check costs nothing, and the call
	 * fails with AH=01h instead. */
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
2:	movb    $0, %cs:JRES_OFF_CALL
	call    jres_up
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

/* A near call: every register into REGS, up into protected mode, the
 * module called (JRES_OFF_CALL: 0 its int13 with REGS, 1 each keyboard
 * entry), back down, every register out of REGS. The caller's SS:SP is
 * saved with this call's return address on it, so the restored stack
 * returns. */
jres_up:
	cli
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
	/* The C the module was compiled from assumes the direction flag
	 * clear, and a real-mode caller may have left it set. */
	cld
	cmpb    $0, JRES_OFF_CALL(%ebx)
	jne     3f
	leal    JRES_OFF_REGS(%ebx), %eax
	pushl   %eax
	call    *JRES_OFF_ENTRY(%ebx)
	jmp     5f
	/* Each keyboard entry in turn; ebx and esi are callee-saved. */
3:	xorl    %esi, %esi
4:	movzbl  JRES_OFF_NKBD(%ebx), %eax
	cmpl    %eax, %esi
	jae     5f
	leal    JRES_OFF_REGS(%ebx), %eax
	pushl   %eax
	call    *JRES_OFF_KBD(%ebx,%esi,4)
	addl    $4, %esp
	incl    %esi
	jmp     4b
	/* ebx is still the block. */
5:	ljmpl   *JRES_OFF_PM16PTR(%ebx)

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
	ret

/* ---- INT 16h: the module's keyboards, for the USB route (DESIGN 13.6) ----
 *
 * The keyboards are polled (hw/usb/hid.c, which writes what it finds into
 * the BIOS's ring and shift flags), then the call goes to the handler this
 * replaced, which answers it from the ring as it would a PS/2 key. AH=01h
 * and 11h poll once. AH=00h and 10h wait, and the BIOS's own wait would
 * never come back to poll again, so they poll until the BIOS's AH=01h or
 * 11h says a key is waiting, interrupts on between polls so a PS/2 key
 * still arrives, and then the BIOS's read takes it. Every other call goes
 * straight through.
 */
	.org    JRES_OFF_CODE16
jres_int16:
	cmpb    $0x01, %ah
	je      2f
	cmpb    $0x11, %ah
	je      2f
	cmpb    $0x00, %ah
	je      1f
	cmpb    $0x10, %ah
	je      1f
	ljmp    *%cs:JRES_OFF_OLDINT16
1:	call    jres_kbd
	pushw   %ax
	incb    %ah                  /* 00h to 01h, 10h to 11h */
	pushfw
	lcall   *%cs:JRES_OFF_OLDINT16
	popw    %ax
	jnz     3f                   /* ZF clear: a key is waiting */
	sti
	jmp     1b
2:	call    jres_kbd
3:	ljmp    *%cs:JRES_OFF_OLDINT16

/* The keyboards polled through jres_up, unless the caller is in v86 mode
 * (as jres_int13), where the BIOS answers from what it has. */
jres_kbd:
	pushw   %ax
	smsw    %ax
	testb   $1, %al
	popw    %ax
	jnz     1f
	movb    $1, %cs:JRES_OFF_CALL
	call    jres_up
1:	ret

/* ---- INT 15h: the memory map, for the USB route (DESIGN Section 13.6) ----
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

/* INT 18h and INT 19h: what a booted medium calls when it has nothing to
 * boot, and what asks for the bootstrap again. Either way the BIOS would go
 * on to its next boot device without a POST, and start it on top of what
 * this route left: INT 13h and 15h hooked into this block, the BDA's memory
 * count lowered, a module in 15-16 MB and, on the PC Card route, a slot
 * still powered with its I/O window open. Windows 2000 started that way on
 * the A21m and bugchecked (STOP 0x0A) on a real-mode vector taken for a
 * kernel pointer. Undoing each change is a list that has to stay complete
 * as jump grows; a cold reset clears all of it, and the machine comes back
 * to the floppy.
 *
 * 0x0000 in the BDA's reset flag (0x472) is a cold start: the full POST,
 * which is what re-initialises the PC Card bridge. Then the reset three
 * ways, as stage2.S's fatal_rm does it. */
	.org    JRES_OFF_RESET
jres_reset:
	cli
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, (0x0472)
	movw    $(jres_reset_msg - jres_blob_start), %si
1:	movb    %cs:(%si), %bl
	testb   %bl, %bl
	jz      3f
	movw    $(SERIAL_PORT + 5), %dx
	movw    $0xFFFF, %cx
2:	inb     %dx, %al
	testb   $0x20, %al           /* the transmitter has room */
	jnz     4f
	loop    2b
4:	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	incw    %si
	jmp     1b
3:	movw    $0xCF9, %dx
	movb    $0x02, %al
	outb    %al, %dx
	movb    $0x06, %al
	outb    %al, %dx
	movl    $500000, %ecx
5:	outb    %al, $0x80
	addr32 loop 5b
	movl    $100000, %ecx
6:	inb     $0x64, %al
	testb   $0x02, %al
	jz      7f
	addr32 loop 6b
7:	movb    $0xFE, %al
	outb    %al, $0x64
	movl    $500000, %ecx
8:	outb    %al, $0x80
	addr32 loop 8b
	ljmp    $0xFFFF, $0x0000     /* the reset vector */
jres_reset_msg:
	.asciz  "falcon-bios: the booted medium returned to the BIOS; resetting\r\n"

/* The release record (biosdefs.h): the magic here, the rest by jres.c. */
	.org    JRES_OFF_MAGIC
	.ascii  "FJRS"
	.org    JRES_OFF_OLDINT18
	.long   0
	.org    JRES_OFF_OLDINT19
	.long   0
	.org    JRES_OFF_OLDKB
	.word   0
	.org    JRES_OFF_OLDNDISK
	.byte   0
	.org    JRES_OFF_OLDINT16
	.long   0
	.org    JRES_OFF_NKBD
	.byte   0
	.org    JRES_OFF_CALL
	.byte   0
	.org    JRES_OFF_KBD
	.fill   JRES_KBD_MAX, 4, 0

	.org    JRES_BYTES - 1
	.byte   0
jres_blob_end:
