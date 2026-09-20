/* falcon-bios stage 1: the master boot record (docs/notes/bios-route.md
 * Sections 4.1 and 5.3).
 *
 * 512 bytes. The BIOS loads this one sector to 0x7C00 in 16-bit real mode
 * with DL holding the drive it came from, and jumps to byte 0. Everything
 * this file does is get stage 2 into memory and jump to it; every decision
 * worth making is made in C, up there, which is Section 5.3's rule.
 *
 * gas rather than NASM (Section 5.2): .S is preprocessed, so the load
 * address, the stage-2 sector count and the serial port live in one header
 * that the C side can include too, rather than being written down twice.
 *
 * WHY THE LBA CALL AND NOT CHS. INT 13h AH=42h (extended read) takes a
 * 64-bit LBA out of a packet in memory and needs no geometry at all. The
 * CHS call it replaced needs cylinders, heads and sectors, which have not
 * described a real disk since the 1990s and which every BIOS lies about
 * differently. 42h has been present since roughly 1996; a machine without it
 * is older than the machines this whole route exists to reach, so it is
 * refused rather than worked around.
 *
 * The drive number is NOT assumed. DL is whatever the BIOS booted from, and
 * on a USB stick that is rarely 0x80; it is saved here and handed to stage 2,
 * which hands it to the read callback.
 */

#include "biosdefs.h"

	.code16
	.section .text.stage1, "ax"
	.globl  _start

_start:
	/* The BIOS may enter with any segment setup and an unknown stack, so
	 * everything is established before a single push. cs is already 0
	 * because the jump target was 0x7C00 in segment 0. */
	cli
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $STAGE1_LOAD, %sp    /* stack grows down from under us */
	cld
	sti

	movb    %dl, boot_drive      /* whatever we were booted from */

	/* Serial first, so everything after this point can be seen. A BIOS
	 * with no video is common on the hardware this targets; one with no
	 * COM1 is not. */
	call    serial_init

	movw    $msg_stage1, %si
	call    serial_puts

	/* Stage 2 comes off the sectors immediately after this one. The
	 * packet is built on the stack rather than in .data: .data would cost
	 * bytes in a 512-byte sector that has none to spare. */
	movw    $dap, %si
	movw    $STAGE2_SECTORS, 2(%si)
	movw    $STAGE2_LOAD, 4(%si)
	movw    $0, 6(%si)           /* segment 0, offset STAGE2_LOAD */
	movl    $STAGE2_LBA, 8(%si)
	movl    $0, 12(%si)

	movb    $0x42, %ah
	movb    boot_drive, %dl
	int     $0x13
	jc      read_failed

	movw    $msg_jump, %si
	call    serial_puts

	/* DL is the contract with stage 2: the drive it must read from. */
	movb    boot_drive, %dl
	ljmp    $0, $STAGE2_LOAD

read_failed:
	movw    $msg_readfail, %si
	call    serial_puts
	/* No retry. A stage 2 that cannot be read is a stick that wants
	 * remaking, and spinning here is more use to somebody with a serial
	 * cable than a reboot loop would be. */
halt:
	hlt
	jmp     halt

/* ---- COM1, polled ----
 *
 * Polled and not interrupt driven on purpose: there is no interrupt handler
 * yet at this point in the boot, and this code runs for microseconds.
 */
serial_init:
	pushw   %ax
	pushw   %dx
	movw    $(SERIAL_PORT + 1), %dx
	movb    $0x00, %al
	outb    %al, %dx             /* interrupts off */
	movw    $(SERIAL_PORT + 3), %dx
	movb    $0x80, %al
	outb    %al, %dx             /* DLAB: the next two are the divisor */
	movw    $SERIAL_PORT, %dx
	movb    $SERIAL_DIVISOR, %al
	outb    %al, %dx
	movw    $(SERIAL_PORT + 1), %dx
	movb    $0x00, %al
	outb    %al, %dx
	movw    $(SERIAL_PORT + 3), %dx
	movb    $0x03, %al
	outb    %al, %dx             /* 8N1, DLAB back off */
	movw    $(SERIAL_PORT + 2), %dx
	movb    $0xC7, %al
	outb    %al, %dx             /* FIFO on, cleared, 14-byte threshold */
	popw    %dx
	popw    %ax
	ret

/* si = NUL-terminated string. Clobbers nothing the caller can see. */
serial_puts:
	pushw   %ax
	pushw   %dx
	pushw   %si
1:	lodsb
	testb   %al, %al
	jz      2f
	movb    %al, %bl
3:	movw    $(SERIAL_PORT + 5), %dx
	inb     %dx, %al
	testb   $0x20, %al           /* transmitter holding register empty */
	jz      3b
	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	jmp     1b
2:	popw    %si
	popw    %dx
	popw    %ax
	ret

boot_drive:
	.byte   0

/* The disk address packet INT 13h AH=42h reads. Sixteen bytes, and the
 * first two are its own size and a reserved zero. */
dap:
	.byte   16
	.byte   0
	.word   0                    /* sectors, filled in above */
	.word   0                    /* offset */
	.word   0                    /* segment */
	.quad   0                    /* LBA */

msg_stage1:
	.asciz  "falcon-bios: stage 1\r\n"
msg_jump:
	.asciz  "falcon-bios: stage 2\r\n"
msg_readfail:
	.asciz  "falcon-bios: stage 2 unreadable (INT 13h AH=42h failed)\r\n"

	/* The signature the BIOS looks for, at the last two bytes of the
	 * sector. The assertion is the point: if anything above grows past
	 * 510 bytes the build fails here rather than producing a sector that
	 * is not bootable. */
	. = _start + 510
	.byte   0x55, 0xAA
