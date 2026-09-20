/* falcon-bios stage 2 (docs/notes/bios-route.md Section 6).
 *
 * Entered by stage 1 in 16-bit real mode at STAGE2_LOAD with DL holding the
 * drive the machine booted from. Its whole job is to reach long mode and read
 * stage 3 in: A20 and the mode switch (lmode.S), the real-mode thunk
 * (thunk.S), and then C, which reads stage 3 off the same disk and jumps to
 * it (load3.c).
 *
 * What stays resident of stage 2 afterwards is the thunk and the gate at the
 * head of this file, because those are the parts that have to be reachable
 * from real mode and stage 3 is not.
 *
 * Everything decidable moves to C as soon as there is a C environment to
 * decide it in, which is Section 5.3's rule and the reason this file is not
 * allowed to grow.
 */

#include "biosdefs.h"

	.code16
	.section .text.stage2, "ax"
	.globl  stage2_start
	.globl  gate_entry
	.globl  gate_boot16
	.globl  gate_target
	.globl  thunk_regs
	.globl  thunk_vector
	.globl  gate_drive
	.globl  putc
	.globl  puts_rm
	.extern lmode_enter
	.extern stage3_load
	.extern boot16
	.extern lmode_entry

/* ---- the gate (biosdefs.h, GATE_*) ----
 *
 * Stage 1 jumps to STAGE2_LOAD, so the head of stage 2 is an address stage 3
 * can be told about without anyone keeping a second number true. What is at
 * it is a jump over the gate; what follows is the gate.
 *
 * .org gives each field the offset biosdefs.h says it has, and the build
 * checks the linked addresses against the header, so the two sides agree by
 * construction. A field that moved without the header moving would be stage 3
 * calling into the middle of the register block.
 *
 * The register block lives here rather than with the thunk that uses it for
 * the same reason the gate does: it has to be at an address that can be
 * written down, and it has to be below a megabyte, because real mode is what
 * fills it in. */
stage2_start:
	jmp     stage2_begin

	.org    0x08
	/* .code64, and it matters: the only caller is stage 3, in long mode.
	 * Assembled in the .code16 this file opens with, gas emits a 16-bit
	 * relative jump, and a processor in long mode reads the same opcode as
	 * a 32-bit one - taking two bytes of .org padding as the top half of
	 * the displacement and jumping somewhere that is not this program. It
	 * cost a reboot loop with no output past the first line of stage 3. */
	.code64
gate_entry:
	jmp     thunk_bios           /* GATE_THUNK */

	.org    0x10
gate_boot16:
	jmp     boot16               /* GATE_BOOT16, and never back */
	.code16

	.org    0x20
thunk_regs:
	.space  32                   /* GATE_REGS */

	.org    0x40
thunk_vector:
	.byte   0                    /* GATE_VECTOR */
gate_drive:
	.byte   0                    /* GATE_DRIVE */

	.org    0x48
gate_target:
	.word   0                    /* GATE_TARGET: offset, then segment */
	.word   0

	.org    0x50
stage2_begin:
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
	/* Into C, with the drive the BIOS handed us. Everything decidable
	 * lives up there (Section 5.3), and this is the last thing the
	 * assembly decides.
	 *
	 * What C does with it is read stage 3 off the same disk and jump to
	 * it. Stage 2 stops here: what stays resident of it is the thunk and
	 * the gate, because those are the parts that have to be reachable
	 * from real mode. */
	movq    $0x7C00, %rsp
	movzbl  drive(%rip), %edi
	call    stage3_load
halt64:halt64:
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
