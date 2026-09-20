/* falcon-bios stage 2, first slice (docs/notes/bios-route.md Section 6).
 *
 * Entered by stage 1 in 16-bit real mode at STAGE2_LOAD with DL holding the
 * drive the machine booted from. This slice exists to prove the chain: the
 * toolchain (gas .S, the linker script, the `bios` build flavour), the
 * handoff from stage 1, and the drive number surviving it.
 *
 * What goes here next, in the order Section 6 stages it: A20, the mode
 * switch, the real-mode thunk, INT 13h/10h/16h behind falcon-core's read
 * callback, and then the menu. Everything decidable moves to C as soon as
 * there is a C environment to decide it in, which is Section 5.3's rule and
 * the reason this file is not allowed to grow.
 */

#include "biosdefs.h"

	.code16
	.section .text.stage2, "ax"
	.globl  stage2_start

stage2_start:
	movb    %dl, drive

	movw    $msg_hello, %si
	call    puts

	/* The drive number, as two hex digits. It is the first thing that
	 * would be wrong if the handoff were broken, and on a USB stick it is
	 * rarely the 0x80 a reader might assume. */
	movw    $msg_drive, %si
	call    puts
	movb    drive, %al
	call    put_hex8
	movw    $msg_crlf, %si
	call    puts

	movw    $msg_done, %si
	call    puts

halt:
	hlt
	jmp     halt

/* al = byte. Prints two hex digits, high nibble first. */
put_hex8:
	pushw   %ax
	pushw   %bx
	movb    %al, %bh
	shrb    $4, %al
	call    put_nibble
	movb    %bh, %al
	andb    $0x0F, %al
	call    put_nibble
	popw    %bx
	popw    %ax
	ret

put_nibble:
	pushw   %ax
	cmpb    $10, %al
	jb      1f
	addb    $('a' - 10), %al
	jmp     2f
1:	addb    $'0', %al
2:	call    putc
	popw    %ax
	ret

/* si = NUL-terminated string. */
puts:
	pushw   %ax
	pushw   %si
1:	lodsb
	testb   %al, %al
	jz      2f
	call    putc
	jmp     1b
2:	popw    %si
	popw    %ax
	ret

/* al = byte. Stage 1 has already initialised the port. */
putc:
	pushw   %ax
	pushw   %bx
	pushw   %dx
	movb    %al, %bl
1:	movw    $(SERIAL_PORT + 5), %dx
	inb     %dx, %al
	testb   $0x20, %al
	jz      1b
	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	popw    %dx
	popw    %bx
	popw    %ax
	ret

drive:
	.byte   0

msg_hello:
	.asciz  "falcon-bios: stage 2 running in real mode\r\n"
msg_drive:
	.asciz  "falcon-bios: boot drive 0x"
msg_crlf:
	.asciz  "\r\n"
msg_done:
	.asciz  "falcon-bios: stage 2 reached its end, halting\r\n"
