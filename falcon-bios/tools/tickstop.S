/* falcon-bios station rig: a floppy boot sector that masks IRQ0 and then
 * boots the first hard disk as the BIOS would (docs/DESIGN.md Section
 * 12.11, the no-ticks case).
 *
 * With IRQ0 masked in the PIC the BIOS's tick count at 0040:006C never
 * moves, which is the state the rig's machine was left in after Windows
 * 2000 reset it: there IRQ0 was pending and never delivered, and the
 * station's countdown, then timed by the ticks alone, stood still. The
 * station on drive 0x80 must still count down and boot its default
 * (console/cdclock.c).
 *
 * It says so on COM1 ("TICKSTOP"), copies itself to 0x600, reads drive
 * 0x80's LBA 0 to 0x7C00 by CHS 0/0/1 and jumps to it with DL 0x80.
 * 8086 real mode, CS 0 and IP 0x7C00 from the BIOS.
 */

#define RELOC 0x600

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
	inb     $0x21, %al
	orb     $0x01, %al
	outb    %al, $0x21
	sti

	movw    $(RELOC + (msg - _start)), %si
1:	lodsb
	testb   %al, %al
	jz      2f
	movw    $0x3FD, %dx
3:	movb    %al, %bl
	inb     %dx, %al
	testb   $0x20, %al
	movb    %bl, %al
	jz      3b
	movw    $0x3F8, %dx
	outb    %al, %dx
	jmp     1b
2:
	movw    $0x0201, %ax             /* read one sector */
	movw    $0x0001, %cx             /* cylinder 0, sector 1 */
	movw    $0x0080, %dx             /* head 0, drive 0x80 */
	movw    $0x7C00, %bx
	int     $0x13
	jc      4f
	movb    $0x80, %dl
	ljmp    $0, $0x7C00
4:	hlt
	jmp     4b

msg:	.asciz  "TICKSTOP: IRQ0 masked, booting drive 0x80\r\n"

	.org    510
	.byte   0x55, 0xAA
