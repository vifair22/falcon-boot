/* falcon-bios stage 2.
 *
 * Entered by stage 1 in 16-bit real mode at STAGE2_LOAD with DL holding the
 * drive the machine booted from. Its whole job is to reach 32-bit protected
 * mode and read stage 3 in: A20 and the mode switch (pmode.S), the real-mode
 * thunk (thunk.S), and then C, which reads stage 3 off the same disk and
 * jumps to it (load3.c).
 *
 * What stays resident of stage 2 afterwards is the thunk and the gate at the
 * head of this file, because those are the parts that have to be reachable
 * from real mode and stage 3 is not.
 *
 * Everything decidable moves to C as soon as there is a C environment to
 * decide it in, which is the reason this file is not allowed to grow.
 */

#include <start/biosdefs.h>

	.code16
	.section .text.stage2, "ax"
	.globl  stage2_start
	.globl  gate_entry
	.globl  gate_magic
	.globl  gate_boot16
	.globl  gate_target
	.globl  gate_s2len
	.globl  thunk_regs
	.globl  thunk_vector
	.globl  gate_drive
	.globl  gate_bootpart
	.globl  gate_marker
	.globl  gate_si
	.globl  putc
	.globl  fatal_rm
	.extern pmode_enter
	.extern stage3_load
	.extern fault_install
	.extern boot16
	.extern pmode_entry

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

	.org    0x04
gate_magic:
	.long   STAGE2_MAGIC         /* GATE_MAGIC */

	.org    0x08
	/* .code32, and it matters: the only caller is stage 3, in protected
	 * mode. Assembled in the .code16 this file opens with, gas emits a
	 * 16-bit relative jump, and a 32-bit processor reads the same opcode
	 * with a 32-bit displacement - taking two bytes of .org padding as its
	 * top half and jumping somewhere that is not this program. It cost a
	 * reboot loop with no output past the first line of stage 3. */
	.code32
gate_entry:
	jmp     thunk_bios           /* GATE_THUNK */

	.org    0x10
gate_boot16:
	jmp     boot16               /* GATE_BOOT16, and never back */
	.code16

	.org    0x18
gate_bootpart:
	.quad   0                    /* GATE_BOOTPART: written by load3.c */

	.org    0x20
thunk_regs:
	.space  32                   /* GATE_REGS */

	.org    0x40
thunk_vector:
	.byte   0                    /* GATE_VECTOR */
gate_drive:
	.byte   0                    /* GATE_DRIVE */
gate_marker:
	.word   0                    /* GATE_MARKER: written by load3.c */
gate_si:
	.word   0                    /* GATE_SI: written by mbr/chain.c */

	.org    0x48
gate_target:
	.word   0                    /* GATE_TARGET: offset, then segment */
	.word   0

	.org    0x4C
gate_s2len:
	.word   0                    /* GATE_S2LEN: written by the build */

	.org    0x50
stage2_begin:
	/* Stage 2 checks itself before it runs any more of itself. Stage 1
	 * reads it off media that can be wrong (a floppy wears out, and reads
	 * back what it read without saying so), and stage 1 has no room for a
	 * check. The build writes stage 2's length into the gate and appends
	 * the CRC-32 of that many bytes (gzip's, reflected 0xEDB88320) after
	 * them; this recomputes it over what stage 1 put in memory. Nothing
	 * above has been written yet, so the bytes are still the disk's.
	 *
	 * Bitwise, no table: a table is a kilobyte stage 2 would carry to save
	 * a few milliseconds once. 32-bit registers in real mode are the
	 * i386's; the i586 floor is checked just after, by code this has
	 * already vouched for. */
	movw    %dx, %bp             /* the boot drive, kept across the loop */
	movw    $STAGE2_LOAD, %si
	movw    gate_s2len, %cx
	jcxz    3f                   /* no length: a build that did not write it */
	movl    $0xFFFFFFFF, %eax
1:	xorb    (%si), %al
	incw    %si
	movb    $8, %bl
2:	shrl    $1, %eax
	jnc     4f
	xorl    $0xEDB88320, %eax
4:	decb    %bl
	jnz     2b
	loop    1b
	notl    %eax
	cmpl    (%si), %eax
	je      5f
3:	movw    $msg_s2crc, %si
	jmp     fatal_rm
5:	movw    %bp, %dx

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

	/* What protected mode and the loader's code need of the machine,
	 * asked before it is tried. These are the two decisions that cannot
	 * wait for C, because what happens without them is not an error but a
	 * fault in real mode: the BIOS's own handler for it returns to the
	 * faulting instruction, and the machine sits there forever. */
	call    cpu_check
	call    mem_check

	/* Into protected mode, and never back. Everything past the jump is a
	 * place C can run: the assembly is transport, and every decision
	 * belongs above it. */
	movw    $msg_pmode, %si
	call    puts

	movl    $stage2_32, pmode_entry
	jmp     pmode_enter

	.code32
stage2_32:
	/* Into C, with the drive the BIOS handed us. Everything decidable
	 * lives up there, and this is the last thing the
	 * assembly decides.
	 *
	 * What C does with it is read stage 3 off the same disk and jump to
	 * it. Stage 2 stops here: what stays resident of it is the thunk and
	 * the gate, because those are the parts that have to be reachable
	 * from real mode. */
	movl    $STAGE2_STACK, %esp  /* biosdefs.h: not below 0x7C00 */
	call    fault_install        /* before anything that can fault */
	movzbl  drive, %eax
	subl    $12, %esp            /* 16-byte aligned at the call */
	pushl   %eax
	call    stage3_load
halt32:
	hlt
	jmp     halt32

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

/* ---- what the machine has to have ----
 *
 * The loader is built for i586 and everything since (DESIGN Section 13.8).
 * What is asked for is CPUID, which every i586 has, which marks the
 * floor better than any instruction the compiler might pick, and which the
 * rest of the loader asks things of
 * (pmode.S about SSE, crypto.c about AES-NI, linux.c about long mode): a
 * processor without it is refused here rather than faulting in whichever
 * of those runs first. tools/check-floor.sh keeps CMOV and the other
 * i686 instructions out of every image the build links.
 */
cpu_check:
	pushfl
	popl    %eax
	movl    %eax, %ecx
	xorl    $0x00200000, %eax    /* EFLAGS.ID: settable iff CPUID exists */
	pushl   %eax
	popfl
	pushfl
	popl    %eax
	pushl   %ecx
	popfl
	xorl    %ecx, %eax
	jz      1f
	ret
1:	movw    $msg_old_cpu, %si
	jmp     fatal_rm

/* The BDA's count of conventional memory, in KiB (0x413). What the BIOS
 * keeps for itself, the EBDA mostly, sits between that and 640 KiB, and
 * the disk bounce buffer below 0x98000 is written without asking. On nearly every machine the count is 639; a
 * board whose EBDA reaches below 608 would have its own data overwritten,
 * and fail somewhere inside the next BIOS call. */
mem_check:
	pushw   %ds
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    (0x0413), %ax
	popw    %ds
	cmpw    $(0x98000 / 1024), %ax
	jb      1f
	ret
1:	movw    $msg_lowmem, %si
	jmp     fatal_rm

/* si = the reason, a whole line. On serial and on the screen, then a key,
 * then a reset: the real-mode counterpart of bios_fatal, for the failures
 * that come before there is a C environment to call it from. Never
 * returns, and never halts, which on a machine with no serial cable would
 * be indistinguishable from a hang. */
fatal_rm:
	sti                          /* INT 16h waits on IRQ1 */
	call    puts
	call    screen_puts
	movw    $msg_key, %si
	call    puts
	call    screen_puts
	xorb    %ah, %ah
	int     $0x16

	/* The reset, three ways, as bios_reboot does it. */
	movw    $0xCF9, %dx
	movb    $0x02, %al
	outb    %al, %dx
	movb    $0x06, %al
	outb    %al, %dx
	call    io_delay
	movl    $100000, %ecx
2:	inb     $0x64, %al
	testb   $0x02, %al
	jz      3f
	addr32 loop 2b
3:	movb    $0xFE, %al
	outb    %al, $0x64
	call    io_delay
	ljmp    $0xFFFF, $0x0000     /* the reset vector */

/* About half a second: port 0x80 takes a microsecond a write on any board. */
io_delay:
	movl    $500000, %ecx
1:	outb    %al, $0x80
	addr32 loop 1b
	ret

/* si = NUL-terminated string, through INT 10h teletype. Preserves si. */
screen_puts:
	pushw   %ax
	pushw   %bx
	pushw   %si
1:	lodsb
	testb   %al, %al
	jz      2f
	movb    $0x0E, %ah
	movw    $0x0007, %bx         /* page 0, light grey */
	int     $0x10
	jmp     1b
2:	popw    %si
	popw    %bx
	popw    %ax
	ret

/* al = byte. Stage 1 has already initialised the port.
 *
 * The wait for the transmitter is bounded, and a port that times out once
 * is written to without waiting from then on: a UART that is present and
 * never empties is the one thing that could stop the boot here, and COM1
 * is a log, not a requirement. A port with nothing behind it reads all ones
 * and passes at once. */
putc:
	pushw   %ax
	pushw   %bx
	pushl   %ecx
	pushw   %dx
	movb    %al, %bl
	cmpb    $0, com_live
	je      2f
	movl    $SERIAL_SPINS, %ecx
	movw    $(SERIAL_PORT + 5), %dx
1:	inb     %dx, %al
	testb   $0x20, %al
	jnz     2f
	addr32 loop 1b
	movb    $0, com_live
2:	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	popw    %dx
	popl    %ecx
	popw    %bx
	popw    %ax
	ret

/* Nonzero until COM1 has timed out once. */
com_live:
	.byte   1

drive:
	.byte   0

msg_hello:
	.asciz  "falcon-bios: stage 2 running in real mode\r\n"
msg_drive:
	.asciz  "falcon-bios: boot drive 0x"
msg_crlf:
	.asciz  "\r\n"
msg_pmode:
	.asciz  "falcon-bios: entering protected mode\r\n"
msg_s2crc:
	.asciz  "falcon-bios: stage 2 did not read back as it was written; the disk may be failing\r\n"
msg_old_cpu:
	.asciz  "falcon-bios: this processor is older than i586, which this loader needs\r\n"
msg_lowmem:
	.asciz  "falcon-bios: the BIOS keeps memory below 608 KiB that this loader uses\r\n"
msg_key:
	.asciz  "press any key to reboot.\r\n"

