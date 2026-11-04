/* falcon-bios station rig: an option ROM that takes the font load away from
 * the VGA BIOS (docs/DESIGN.md Section 7.10, the spin-nofont case).
 *
 * SeaVGABIOS built for coreboot's graphics init returns from INT 10h AX=1100h
 * having loaded nothing, and QEMU's VGA BIOS does load. This hooks INT 10h,
 * returns from AX=1100h the same way and passes everything else to the handler
 * it replaced, whose address it keeps in the shadowed ROM, writable while
 * SeaBIOS runs the option ROMs.
 *
 * 55 AA, one 512-byte unit, the init entry at offset 3 that SeaBIOS far-calls
 * in real mode; the build writes the last byte so the sum is zero.
 */

	.code16
	.section .text, "ax"
	.globl  _start
_start:
	.byte   0x55, 0xAA
	.byte   1
init:  /* falcon-asm: reached=offset: the option ROM's entry, at offset 3 */
	pushw   %ax
	pushw   %ds
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    (0x10 * 4), %ax
	movw    %ax, %cs:old10
	movw    (0x10 * 4 + 2), %ax
	movw    %ax, %cs:old10 + 2
	cli
	movw    $filter, (0x10 * 4)
	movw    %cs, (0x10 * 4 + 2)
	sti
	popw    %ds
	popw    %ax
	lret

filter:
	cmpw    $0x1100, %ax
	jne     1f
	iret
1:	ljmp    *%cs:old10

old10:	.long   0

	.org    511
	.byte   0                         /* the checksum, written by the build */
