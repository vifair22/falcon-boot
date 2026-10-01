/* falcon-bios: the drive swap, the station's one resident (docs/DESIGN.md
 * Section 12.4; installed by mbr/chain.c).
 *
 * WHY THIS EXISTS. DOS, Windows 9x and NTLDR's boot.ini (`rdisk(N)`) name
 * disks by BIOS order and expect the disk they boot from to be 0x80,
 * because a BIOS booting that disk directly would have made it so. When the
 * station chains a second disk (its MBR, or a partition on it), this hook
 * makes it so: every INT 13h call for that disk's number goes to 0x80 and
 * every call for 0x80 goes to that disk. GRUB's drivemap is the same idea.
 *
 * WHAT IT DECIDES: one comparison. A call whose DL is neither of the two
 * numbers goes through unchanged; one whose DL is either goes to the other.
 * The previous handler is called, not jumped to, because DL has to be put
 * back afterwards: a caller that reads DL after the call (a loop over
 * drives, a saved drive number) must find the number it passed, not the one
 * the call was redirected to. Two functions return something else in DL and
 * keep it: AH=08h (the drive count) and AH=15h (the low word of a sector
 * count). The flags are the previous handler's, carry and zero included,
 * with the caller's interrupt flag kept.
 *
 * What it does not do: the BDA's disk count and the fixed disk parameter
 * tables behind INT 41h and 46h are left as the BIOS set them. Both disks
 * exist either way, so the count is right, and nothing in scope reads the
 * tables for a disk it can ask INT 13h about.
 *
 * POSITION INDEPENDENCE, as vcdres.S: the block is copied to whatever
 * segment mem/lowmem.c leaves room at, so its two fields are reached as
 * numeric offsets from biosdefs.h with a CS override, and every jump is
 * relative. The block is SWAP_OFF_CODE plus the handler, under a hundred
 * bytes; the build checks it fits SWAP_BYTES.
 */

#include <start/biosdefs.h>

	.code16
	.section .text.swap, "ax"
	.globl  swap_blob_start
	.globl  swap_blob_end

swap_blob_start:
	.org    SWAP_OFF_OLDINT13
	.long   0
	.org    SWAP_OFF_DRIVE
	.byte   0

	.org    SWAP_OFF_CODE
	.globl  swap_entry
swap_entry:
	pushw   %bp
	movw    %sp, %bp             /* 2(%bp) IP, 4(%bp) CS, 6(%bp) FLAGS */
	pushw   %dx                  /* -2(%bp): the caller's DX */
	pushw   %ax                  /* -4(%bp): the caller's AX; -3 is AH */

	cmpb    %cs:SWAP_OFF_DRIVE, %dl
	jne     1f
	movb    $0x80, %dl
	jmp     2f
1:	cmpb    $0x80, %dl
	jne     2f
	movb    %cs:SWAP_OFF_DRIVE, %dl

	/* An int, by hand: the flags, interrupts off, a far call the previous
	 * handler's iret returns from. */
2:	pushfw
	cli
	lcall   *%cs:SWAP_OFF_OLDINT13

	/* Its flags into the caller's frame, all but IF, which stays the
	 * caller's: the iret below restores what the caller had. */
	pushw   %ax                  /* -6(%bp): the result's AX */
	pushfw
	popw    %ax
	andw    $0xFDFF, %ax
	andw    $0x0200, 6(%bp)
	orw     %ax, 6(%bp)

	/* The caller's DL back, but where the call answers in it. */
	movb    -3(%bp), %ah
	cmpb    $0x08, %ah
	je      3f
	cmpb    $0x15, %ah
	je      3f
	movb    -2(%bp), %dl
3:	popw    %ax
	movw    %bp, %sp
	popw    %bp
	iret

swap_blob_end:
