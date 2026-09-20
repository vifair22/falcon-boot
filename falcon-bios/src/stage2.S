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
	.globl  putc
	.globl  puts_rm
	.extern lmode_enter
	.extern thunk_bios
	.extern thunk_vector
	.extern thunk_regs
	.extern lmode_entry

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

	/* Into long mode, and never back. Everything past the jump is a place
	 * C can run, which is Section 5.3's rule: the assembly is transport,
	 * and every decision belongs above it. */
	movw    $msg_lmode, %si
	call    puts

	movl    $stage2_64, %eax
	movl    %eax, lmode_entry
	movl    $0, lmode_entry + 4
	jmp     lmode_enter

	.code64
/* The first code in this project to run in 64-bit mode. It prints, which is
 * the only claim it is making, and halts. */
stage2_64:
	movq    $msg_in64, %rsi
	call    puts64

	/* Read the boot sector back, from 64-bit code, through the BIOS.
	 * Nothing else proves the thunk: the processor has to go all the way
	 * down to real mode, let the firmware touch the disk, and come back
	 * with the answer and its own state intact. The sector read is the
	 * one this machine booted from, so its last two bytes are known. */
	movq    $msg_thunk, %rsi
	call    puts64

	/* The disk address packet, where real mode can reach it. */
	movb    $16, dap(%rip)
	movb    $0, dap + 1(%rip)
	movw    $1, dap + 2(%rip)          /* one sector */
	movw    $sector, dap + 4(%rip)     /* offset */
	movw    $0, dap + 6(%rip)          /* segment */
	movq    $0, dap + 8(%rip)          /* LBA 0: the boot sector */

	movl    $0x4200, thunk_regs(%rip)          /* ah=42 */
	movzbl  drive(%rip), %eax
	movl    %eax, thunk_regs + 12(%rip)        /* dl = drive */
	movl    $dap, thunk_regs + 16(%rip)        /* ds:si -> packet */
	movw    $0, thunk_regs + 24(%rip)          /* es */
	movb    $0x13, thunk_vector(%rip)
	call    thunk_bios

	/* Carry means the BIOS refused. */
	movw    thunk_regs + 28(%rip), %ax
	testw   $1, %ax
	jnz     thunk_failed

	movw    sector + 510(%rip), %ax
	cmpw    $0xAA55, %ax
	jne     thunk_bad

	movq    $msg_thunk_ok, %rsi
	call    puts64
	jmp     thunk_end

thunk_failed:
	movq    $msg_thunk_cf, %rsi
	call    puts64
	jmp     thunk_end
thunk_bad:
	movq    $msg_thunk_sig, %rsi
	call    puts64

thunk_end:
	movq    $msg_done, %rsi
	call    puts64
halt64:
	hlt
	jmp     halt64

/* al = byte. */
putc64:
	pushq   %rax
	pushq   %rbx
	pushq   %rdx
	movb    %al, %bl
1:	movw    $(SERIAL_PORT + 5), %dx
	inb     %dx, %al
	testb   $0x20, %al
	jz      1b
	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	popq    %rdx
	popq    %rbx
	popq    %rax
	ret

/* rsi = NUL-terminated string. The port writes are identical to the 16-bit
 * versions above; only the registers holding the pointer are wider. */
puts64:
	pushq   %rax
	pushq   %rbx
	pushq   %rdx
	pushq   %rsi
1:	lodsb
	testb   %al, %al
	jz      3f
	movb    %al, %bl
2:	movw    $(SERIAL_PORT + 5), %dx
	inb     %dx, %al
	testb   $0x20, %al
	jz      2b
	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	jmp     1b
3:	popq    %rsi
	popq    %rdx
	popq    %rbx
	popq    %rax
	ret

	.code16

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
puts_rm:
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
msg_lmode:
	.asciz  "falcon-bios: entering long mode\r\n"
msg_in64:
	.asciz  "falcon-bios: running in 64-bit long mode\r\n"
msg_thunk:
	.asciz  "falcon-bios: reading LBA 0 through the BIOS from long mode\r\n"
msg_thunk_ok:
	.asciz  "falcon-bios: thunk ok, boot sector signature 0xAA55\r\n"
msg_thunk_cf:
	.asciz  "falcon-bios: thunk returned carry: the BIOS refused the read\r\n"
msg_thunk_sig:
	.asciz  "falcon-bios: thunk returned, but the sector is not a boot sector\r\n"
msg_done:
	.asciz  "falcon-bios: stage 2 reached its end, halting\r\n"

	.balign 4
dap:
	.space  16
	.balign 16
sector:
	.space  512
