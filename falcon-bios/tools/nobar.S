/* falcon jump's rig: a boot sector that leaves the CardBus bridge as a BIOS
 * that does not know CardBus does (docs/DESIGN.md Section 13.9, the
 * cf-nobar case).
 *
 * SeaBIOS assigns every BAR, so the rig cannot otherwise show PCC.MOD a
 * bridge whose socket registers were never placed on the bus, which is how
 * the ThinkPad A21m's BIOS leaves its second socket. Booted first (-boot
 * order=c, this as the disk's sector 0), it finds every CardBus bridge on
 * bus 0 through configuration mechanism #1 (class 0x0607), writes 0 to its
 * BAR0 and clears memory decoding in its command register, says so on COM1
 * ("NOBAR"), then reads the floppy's sector 0 to 0x7C00 and jumps to it
 * with DL 0, as the BIOS would have booted it. Copied to 0x600 first, as
 * tickstop.S is, since the floppy's sector lands where this one is. 8086
 * real mode with 386 operands for the configuration ports, CS 0 and IP
 * 0x7C00 from the BIOS.
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
	sti
	/* Bus 0, every device and function: %ebx is the mechanism #1 address
	 * of the function's register 0. */
	movl    $0x80000000, %ebx
1:	movl    %ebx, %eax
	call    cfg_read                 /* vendor and device */
	cmpw    $0xFFFF, %ax
	je      3f
	movl    %ebx, %eax
	orb     $0x08, %al
	call    cfg_read                 /* class code */
	shrl    $16, %eax
	cmpw    $0x0607, %ax
	jne     3f
	movl    %ebx, %eax
	orb     $0x10, %al
	movw    $0xCF8, %dx
	outl    %eax, %dx
	xorl    %eax, %eax
	movw    $0xCFC, %dx
	outl    %eax, %dx                /* BAR0 = 0 */
	movl    %ebx, %eax
	orb     $0x04, %al
	call    cfg_read
	andb    $0xFD, %al               /* memory space decoding off */
	movw    $0xCFC, %dx
	outl    %eax, %dx
	movw    $(RELOC + (cleared - _start)), %si
	call    puts
3:	addl    $0x100, %ebx
	cmpl    $0x80010000, %ebx
	jb      1b

	movw    $(RELOC + (msg - _start)), %si
	call    puts
	movw    $0x0201, %ax             /* read one sector */
	movw    $0x0001, %cx             /* cylinder 0, sector 1 */
	movw    $0x0000, %dx             /* head 0, drive 0x00 */
	movw    $0x7C00, %bx
	int     $0x13
	jc      4f
	movb    $0x00, %dl
	ljmp    $0, $0x7C00
4:	hlt
	jmp     4b

/* The dword of the configuration register %eax addresses, in %eax. */
cfg_read:
	movw    $0xCF8, %dx
	outl    %eax, %dx
	movw    $0xCFC, %dx
	inl     %dx, %eax
	ret

/* The NUL-terminated string at %si on COM1. %ah holds the byte while the
 * transmitter is polled; %bx is the scan's. */
puts:
	lodsb
	testb   %al, %al
	jz      2f
	movb    %al, %ah
	movw    $0x3FD, %dx
5:	inb     %dx, %al
	testb   $0x20, %al
	jz      5b
	movb    %ah, %al
	movw    $0x3F8, %dx
	outb    %al, %dx
	jmp     puts
2:	ret

cleared: .asciz "NOBAR: a CardBus bridge's BAR0 cleared, memory decoding off\r\n"
msg:	.asciz  "NOBAR: booting drive 0x00\r\n"

	.org    510
	.byte   0x55, 0xAA
