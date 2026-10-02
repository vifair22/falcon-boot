/* falcon-bios jump rig: a boot sector that takes INT 15h E820 away from
 * the BIOS and then boots the floppy as the BIOS would (docs/DESIGN.md
 * Section 13.9, the noe820 cases).
 *
 * SeaBIOS always has E820; the machines falcon jump is for (Section 13)
 * mostly do not, and answer the older questions, AX=E801 and AH=88h,
 * instead. This installs an INT 15h filter that fails AX=E820 with carry
 * set and AH=86h, as a BIOS without the call does, and passes everything
 * else to the handler it replaced. With the byte at MODE_OFF nonzero
 * (the rig patches it) it fails AX=E801 the same way, so the loader's
 * AH=88h path runs.
 *
 * Booted as the hard disk's MBR (QEMU -boot c), it copies itself to 0x600,
 * where it stays for as long as the vector points at it, says so on COM1
 * ("NOE820"), reads the floppy's sector 0 to 0x7C00 by CHS 0/0/1 (three
 * tries, a reset between, as a BIOS gives a floppy) and jumps to it with
 * DL 0. 8086 real mode, CS 0 and IP 0x7C00 from the BIOS.
 */

#define RELOC    0x600
#define MODE_OFF 508

	.code16
	.section .text, "ax"
	.globl  _start
_start:
	cli
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $0x7C00, %sp
	cld
	movw    $0x7C00, %si
	movw    $RELOC, %di
	movw    $256, %cx
	rep movsw
	ljmp    $0, $(RELOC + (moved - _start))
moved:
	movl    (0x15 * 4), %eax
	movl    %eax, (RELOC + (old15 - _start))
	movw    $(RELOC + (filter - _start)), (0x15 * 4)
	movw    $0, (0x15 * 4 + 2)
	sti

	movw    $(RELOC + (msg - _start)), %si
	call    puts
	cmpb    $0, (RELOC + (mode - _start))
	je      1f
	movw    $(RELOC + (msg88 - _start)), %si
	call    puts
1:	movw    $(RELOC + (crlf - _start)), %si
	call    puts

	movw    $3, %cx
2:	pushw   %cx
	xorw    %ax, %ax                 /* reset the floppy */
	xorw    %dx, %dx
	int     $0x13
	movw    $0x0201, %ax             /* read one sector */
	movw    $0x0001, %cx             /* cylinder 0, sector 1 */
	movw    $0x0000, %dx             /* head 0, drive 0x00 */
	movw    $0x7C00, %bx
	int     $0x13
	popw    %cx
	jnc     3f
	loop    2b
4:	hlt
	jmp     4b
3:	xorb    %dl, %dl
	ljmp    $0, $0x7C00

/* si = the string, on COM1, polled. */
puts:
	lodsb
	testb   %al, %al
	jz      2f
	movw    $0x3FD, %dx
1:	movb    %al, %bl
	inb     %dx, %al
	testb   $0x20, %al
	movb    %bl, %al
	jz      1b
	movw    $0x3F8, %dx
	outb    %al, %dx
	jmp     puts
2:	ret

/* INT 15h. The stack on entry is IP, CS, FLAGS; the carry the caller sees
 * is the one in the saved flags, so a failure sets it there. */
filter:
	cmpw    $0xE820, %ax
	je      fail
	cmpb    $0, %cs:(RELOC + (mode - _start))
	je      pass
	cmpw    $0xE801, %ax
	je      fail
pass:
	ljmp    *%cs:(RELOC + (old15 - _start))
fail:
	pushw   %bp
	movw    %sp, %bp
	orw     $1, 6(%bp)
	popw    %bp
	movb    $0x86, %ah
	iret

old15:	.long   0
msg:	.asciz  "NOE820: INT 15h E820 fails, booting the floppy"
msg88:	.asciz  "; E801 fails too"
crlf:	.asciz  "\r\n"

	.org    MODE_OFF
mode:	.byte   0
	.org    510
	.byte   0x55, 0xAA
