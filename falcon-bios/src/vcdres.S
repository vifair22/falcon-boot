/* falcon-bios: the resident INT 13h handler that serves an ISO as a CD
 * (docs/notes/bios-route.md Sections 4.1 and 7.1).
 *
 * WHY THIS EXISTS. An ISO's own bootloader - isolinux, the ISO's GRUB,
 * FreeBSD's loader, Windows' bootmgr - reads its medium through INT 13h and
 * has no other way to. Under UEFI falcon-boot answers that need by
 * publishing a Block I/O protocol over the ISO; under BIOS the equivalent is
 * a resident handler on a drive number that did not exist, which is this.
 *
 * WHAT IT IS ALLOWED TO DECIDE: as little as possible. It is 16-bit
 * real-mode assembly, which is the one part of this tree that no analyzer
 * and no test reaches (Section 7.2), so the arithmetic it performs is worked
 * out in vcdcore.c, tested there, and handed over as a table. What is left
 * here is a linear scan, one multiply, one add, and a re-issued read.
 *
 * POSITION INDEPENDENCE, AND WHY NOTHING HERE IS NAMED BY SYMBOL. The block
 * is copied to wherever shrinking the BIOS data area's memory count leaves
 * room, so nothing may assume an address. A symbol would be the linked
 * address of this code inside stage 3 - above a megabyte - truncated to
 * sixteen bits, which is a number with no meaning at all. So every field is
 * reached as a numeric offset from biosdefs.h with a CS override, `.org`
 * places each one at that offset, and the build checks the two agree. Every
 * jump within the block is relative and needs no such care.
 *
 * THE BLOCK:
 *
 *   0x0000  the previous INT 13h vector, which every call we do not answer
 *           jumps to and every read we do answer goes through
 *   0x0004  the drive we answer for, and the physical drive behind it
 *   0x0006  the run count, the disk sectors per CD sector, the medium size
 *   0x0010  our own disk address packet: the caller's may not be touched,
 *           because a caller may reuse it
 *   0x0024  the El Torito status packet AH=4Bh returns
 *   0x0040  the runs, as vcdcore.c built them
 *   0x0440  this code
 */

#include "biosdefs.h"

	.code16
	.section .text.vcd, "ax"
	.globl  vcd_blob_start
	.globl  vcd_blob_end

vcd_blob_start:
	.org    VCD_OFF_OLDINT13
	.long   0
	.org    VCD_OFF_DRIVE
	.byte   0
	.byte   0
	.org    VCD_OFF_NRUNS
	.word   0
	.org    VCD_OFF_PERCD
	.word   0
	.org    VCD_OFF_NCD
	.long   0
	.org    VCD_OFF_NOVER
	.word   0
	.org    VCD_OFF_DAP
	.space  16
	.org    VCD_OFF_END
	.word   0
	.org    VCD_OFF_GAP
	.long   0
	.org    VCD_OFF_NREAL
	.long   0
	.org    VCD_OFF_NLIMIT
	.long   0
	.org    VCD_OFF_OVER
	.space  (VCD_MAX_OVER_ASM * VCD_OV_LEN)
	.org    VCD_OFF_MOVEGDT
	.space  48
	.org    VCD_OFF_RUNS
	.space  (VCD_MAX_RUNS_ASM * VCD_RUN_LEN)
	.org    VCD_OFF_SPEC
	.space  20

	.org    VCD_OFF_LSRC
	.word   0
	.word   0
	.word   0
	.byte   0

/* ---- the launch stub ----
 *
 * The last thing this loader runs. It moves the medium's own boot image from
 * where stage 3 staged it to where El Torito says it goes, hands over the
 * drive number in DL, and jumps.
 *
 * It lives in the resident rather than in stage 2 because the destination is
 * 0x7C00 and everything above that is stage 2: the gate, the thunk, and the
 * code that would be doing the copying. Here it is in memory the BIOS has
 * been told does not exist, so what it copies cannot reach it.
 */
	.org    VCD_OFF_LAUNCH
	.globl  vcd_launch
vcd_launch:
	cli
	movw    %cs:VCD_OFF_LSRC, %ax
	movw    %ax, %ds
	xorw    %si, %si
	movw    %cs:VCD_OFF_LDST, %ax
	movw    %ax, %es
	xorw    %di, %di
	movw    %cs:VCD_OFF_LLEN, %cx
	cld
	rep movsb

	/* DL is the contract: the program about to run reads the drive it
	 * came off it and has no other way to know. */
	movb    %cs:VCD_OFF_LDRIVE, %dl
	movw    %cs:VCD_OFF_LDST, %ax
	pushw   %ax
	pushw   $0
	sti
	lret

	.org    VCD_OFF_CODE
/* ---- the handler ---- */
	.globl  vcd_entry
vcd_entry:
	/* Not ours: straight on to whoever had the vector before us. A jump
	 * rather than a call, so that handler's iret returns to the original
	 * caller and we are not on the path at all. */
	cmpb    %dl, %cs:VCD_OFF_DRIVE
	je      1f
	ljmp    *%cs:VCD_OFF_OLDINT13

1:	cmpb    $0x42, %ah
	je      do_read
	cmpb    $0x41, %ah
	je      do_extcheck
	cmpb    $0x48, %ah
	je      do_params
	cmpb    $0x4B, %ah
	je      do_emul
	cmpb    $0x00, %ah
	je      ok_zero
	cmpb    $0x15, %ah
	je      do_type
	cmpb    $0x43, %ah
	je      write_protected

	/* Everything else: invalid command. */
	movb    $0x01, %ah
	jmp     fail

write_protected:
	/* Every write. A medium this loader serves is read-only, and says so
	 * in the one way the interface has. */
	movb    $0x03, %ah
	jmp     fail

ok_zero:
	xorb    %ah, %ah
	jmp     succeed

/* AH=41h: are the extensions here. They are the only way this drive can be
 * read at all, since a CD has no geometry to speak of. */
do_extcheck:
	movw    $0xAA55, %bx
	movw    $0x0005, %cx      /* packet access, and EDD */
	movb    $0x30, %ah        /* EDD 3.0 */
	jmp     succeed

/* AH=15h: what kind of drive, with its size in CX:DX counted in the
 * 512-byte sectors this call is defined in. */
do_type:
	pushl   %eax
	movl    %cs:VCD_OFF_NCD, %eax
	shll    $2, %eax          /* CD sectors to 512-byte sectors */
	movw    %ax, %dx
	shrl    $16, %eax
	movw    %ax, %cx
	popl    %eax
	movb    $0x03, %ah
	jmp     succeed

/* AH=48h: the drive parameters, into the caller's buffer at DS:SI. The two
 * fields that matter are the sector size, which is what makes this a CD, and
 * the sector count. */
do_params:
	pushw   %di
	pushw   %es
	pushw   %ds
	popw    %es
	movw    %si, %di
	/* The caller says how much room it has; below the 26-byte form is a
	 * caller we cannot answer. */
	cmpw    $26, %es:(%di)
	jb      2f
	movw    $26, %es:(%di)
	movw    $0x0004, %es:2(%di)   /* geometry not valid: this is a CD */
	movl    $0xFFFFFFFF, %es:4(%di)
	movl    $0xFFFFFFFF, %es:8(%di)
	movl    $0xFFFFFFFF, %es:12(%di)
	pushl   %eax
	movl    %cs:VCD_OFF_NCD, %eax
	movl    %eax, %es:16(%di)
	xorl    %eax, %eax
	movl    %eax, %es:20(%di)
	popl    %eax
	movw    $VCD_CD_BYTES, %es:24(%di)
	popw    %es
	popw    %di
	xorb    %ah, %ah
	jmp     succeed
2:	popw    %es
	popw    %di
	movb    $0x01, %ah
	jmp     fail

/* AH=4Bh AL=01: the El Torito status packet, which is how a bootloader
 * started from a CD finds out which drive it came off and where its boot
 * image is. isolinux asks this first and gives up if it fails. */
do_emul:
	cmpb    $0x01, %al
	jne     3f
	pushw   %si
	pushw   %di
	pushw   %cx
	pushw   %ds
	pushw   %es
	pushw   %ds
	popw    %es
	movw    %si, %di
	pushw   %cs
	popw    %ds
	movw    $VCD_OFF_SPEC, %si
	movw    $0x13, %cx
	cld
	rep movsb
	popw    %es
	popw    %ds
	popw    %cx
	popw    %di
	popw    %si
	xorb    %ah, %ah
	jmp     succeed
3:	movb    $0x01, %ah
	jmp     fail

/* AH=42h: the read. DS:SI is the caller's disk address packet; the answer is
 * one re-issued read per run the request spans.
 *
 * The caller's packet is never written to. A caller that reuses it - and
 * GRUB does - would otherwise see its own request rewritten underneath it. */
do_read:
	pushal
	pushw   %ds
	pushw   %es

	movzwl  2(%si), %ecx      /* CD sectors wanted */
	movl    8(%si), %ebx      /* the CD LBA; the high half is not ours */
	movw    6(%si), %di       /* buffer segment */
	movw    4(%si), %ax       /* buffer offset */
	movw    %ax, %cs:(VCD_OFF_DAP + 4)
	testl   %ecx, %ecx
	jz      read_done

read_loop:
	pushw   %cs
	popw    %ds

	/* ---- is this stretch served from memory? ----
	 *
	 * The overlay: a short table of CD sector ranges whose bytes are in
	 * conventional memory rather than on the disk, which is what makes a
	 * medium bootable that is not bootable as it lies on the stick
	 * (media/bsdmd.c's two edits are the first to need it). Under UEFI
	 * this is iso/overlay.h.
	 *
	 * The scan answers two questions at once: does an entry contain EBX,
	 * and if not, how far is the nearest one above it. The second matters
	 * as much as the first, because a disk read may not run into an
	 * override: the handler issues one BIOS call per pass. */
	movl    %ecx, %cs:VCD_OFF_GAP
	/* A pass may not span the image's end either: past it the medium is
	 * padding, and the two are served by different code. */
	movl    %cs:VCD_OFF_NREAL, %eax
	cmpl    %ebx, %eax
	jbe     1f
	subl    %ebx, %eax
	cmpl    %cs:VCD_OFF_GAP, %eax
	jae     1f
	movl    %eax, %cs:VCD_OFF_GAP
1:	movzwl  %cs:VCD_OFF_NOVER, %eax
	testl   %eax, %eax
	jz      find_run_start
	shlw    $4, %ax
	addw    $VCD_OFF_OVER, %ax
	movw    %ax, %cs:VCD_OFF_END
	movw    $VCD_OFF_OVER, %si
ov_next:
	cmpw    %cs:VCD_OFF_END, %si
	jae     find_run_start
	movl    (%si), %eax
	cmpl    %eax, %ebx
	jb      ov_above
	movl    %eax, %edx
	addl    4(%si), %edx
	cmpl    %edx, %ebx
	jb      ov_hit
	jmp     ov_cont
ov_above:
	subl    %ebx, %eax
	cmpl    %cs:VCD_OFF_GAP, %eax
	jae     ov_cont
	movl    %eax, %cs:VCD_OFF_GAP
ov_cont:
	addw    $VCD_OV_LEN, %si
	jmp     ov_next

/* EDX is one past the override's last sector and SI is the entry. */
ov_hit:
	movl    %edx, %eax
	subl    %ebx, %eax
	cmpl    %ecx, %eax
	jbe     5f
	movl    %ecx, %eax
5:	/* Capped at 16 sectors a pass: the copy below addresses with SI and
	 * DI, which are sixteen bits, and 32 KB is the most that cannot
	 * wrap. */
	cmpl    $16, %eax
	jbe     6f
	movl    $16, %eax
6:	movl    %eax, %edx

	testw   $VCD_OV_EXT, 10(%si)
	jnz     ov_ext

	/* The source: the override's segment, advanced 128 paragraphs for
	 * every sector of it already served. */
	movl    %ebx, %eax
	subl    (%si), %eax
	shll    $7, %eax
	addw    8(%si), %ax

	pushl   %ecx
	pushw   %si
	pushw   %di
	pushw   %ds
	pushw   %es
	movw    %ax, %ds
	xorw    %si, %si
	movw    %di, %es
	movw    %cs:(VCD_OFF_DAP + 4), %di
	movl    %edx, %ecx
	shll    $9, %ecx             /* 2048 bytes is 512 dwords */
	cld
	rep movsl
	popw    %es
	popw    %ds
	popw    %di
	popw    %si
	popl    %ecx
	jmp     advance

/* ---- an override the far side of a megabyte ----
 *
 * Windows is the only thing that needs this: its patched boot.wim appends a
 * tail measured in megabytes, which is more conventional memory than a
 * machine has to give. Real mode cannot address it, so the BIOS is asked to
 * move it instead - INT 15h AH=87h, which is the one service that has always
 * been able to. It reads six descriptors; the second and third are the
 * source and the destination and are the only ones this writes.
 *
 * EDX is the pass count and SI the entry. */
ov_ext:
	pushl   %ecx
	pushw   %di

	/* EAX: the source, the override's physical address advanced by what
	 * has already been served out of it. */
	movl    %ebx, %eax
	subl    (%si), %eax
	shll    $11, %eax            /* CD sectors to bytes */
	addl    12(%si), %eax

	/* ECX: the destination, which is a segment and an offset until it is
	 * a linear address. */
	movzwl  %di, %ecx
	shll    $4, %ecx
	movzwl  %cs:(VCD_OFF_DAP + 4), %ebp
	addl    %ebp, %ecx

	/* EBP: bytes this pass. AH=87h counts words and CX is sixteen bits,
	 * so 64 KB is its limit; sixteen CD sectors is half that. */
	movl    %edx, %ebp
	shll    $11, %ebp

	/* The source descriptor, then the destination, in the shape a
	 * protected-mode segment descriptor has: limit, then the base in
	 * three pieces with an access byte wedged between them. */
	pushl   %ebp
	decl    %ebp
	movw    %bp, %cs:(VCD_OFF_MOVEGDT + 16)
	movw    %bp, %cs:(VCD_OFF_MOVEGDT + 24)
	popl    %ebp
	movw    %ax, %cs:(VCD_OFF_MOVEGDT + 18)
	movw    %cx, %cs:(VCD_OFF_MOVEGDT + 26)
	movb    $0x93, %cs:(VCD_OFF_MOVEGDT + 21)
	movb    $0x93, %cs:(VCD_OFF_MOVEGDT + 29)
	movb    $0, %cs:(VCD_OFF_MOVEGDT + 22)
	movb    $0, %cs:(VCD_OFF_MOVEGDT + 30)
	shrl    $16, %eax
	movb    %al, %cs:(VCD_OFF_MOVEGDT + 20)
	movb    %ah, %cs:(VCD_OFF_MOVEGDT + 23)
	shrl    $16, %ecx
	movb    %cl, %cs:(VCD_OFF_MOVEGDT + 28)
	movb    %ch, %cs:(VCD_OFF_MOVEGDT + 31)

	pushw   %es
	pushw   %si
	pushw   %cs
	popw    %es
	movw    $VCD_OFF_MOVEGDT, %si
	movl    %ebp, %ecx
	shrl    $1, %ecx             /* words, which is what AH=87h counts */
	movb    $0x87, %ah
	int     $0x15
	popw    %si
	popw    %es

	popw    %di
	popl    %ecx
	jc      read_fail
	jmp     advance

find_run_start:
	/* Which run holds EBX, and how many sectors of the request are in it.
	 * The scan is linear because the table is short by construction: a
	 * medium in more pieces than that is one vcdcore.c refused. */
	movw    $VCD_OFF_RUNS, %si
	movw    %cs:VCD_OFF_NRUNS, %ax
	testw   %ax, %ax
	jz      no_run
	movw    %ax, %bp

find_run:
	movl    (%si), %eax       /* the run's first CD sector */
	cmpl    %eax, %ebx
	jb      next_run
	movl    %eax, %edx
	addl    4(%si), %edx      /* one past its last */
	cmpl    %edx, %ebx
	jb      found_run
next_run:
	addw    $VCD_RUN_LEN, %si
	decw    %bp
	jnz     find_run

/* ---- past the image, inside the medium ----
 *
 * The tail padding, which no run covers and which reads as zeros. It is
 * here because a BIOS caller reads in fixed-size chunks and the last one
 * runs past the end of an image whose length is not a multiple of that
 * chunk: FreeBSD's loader reads eight sectors at a time and asked for one
 * past the end of a 521 MB medium, and refusing that read is refusing the
 * medium. A pressed CD is padded to a whole number of blocks and answers. */
no_run:
	cmpl    %cs:VCD_OFF_NLIMIT, %ebx
	jae     read_fail
	movl    %cs:VCD_OFF_NLIMIT, %edx
	subl    %ebx, %edx
	cmpl    %cs:VCD_OFF_GAP, %edx
	jbe     10f
	movl    %cs:VCD_OFF_GAP, %edx
10:	cmpl    $16, %edx
	jbe     11f
	movl    $16, %edx
11:
	pushl   %ecx
	pushw   %di
	pushw   %es
	movw    %di, %es
	movw    %cs:(VCD_OFF_DAP + 4), %di
	movl    %edx, %ecx
	shll    $9, %ecx             /* 2048 bytes is 512 dwords */
	xorl    %eax, %eax
	cld
	rep stosl
	popw    %es
	popw    %di
	popl    %ecx
	jmp     advance

found_run:
	/* EAX: how far into the run. The only arithmetic in this file. */
	movl    %ebx, %eax
	subl    (%si), %eax

	/* EDX: this pass's count, the run's remainder capped by what the
	 * request wants and by where the next override starts. */
	movl    4(%si), %edx
	subl    %eax, %edx
	cmpl    %cs:VCD_OFF_GAP, %edx
	jbe     4f
	movl    %cs:VCD_OFF_GAP, %edx
4:	/* And capped at sixteen CD sectors a pass, which is sixty-four disk
	 * sectors. AH=42h is documented to carry up to 127 and plenty of
	 * firmware refuses more; a caller asking for a megabyte gets it in
	 * passes rather than one call no BIOS promised to answer. */
	cmpl    $16, %edx
	jbe     7f
	movl    $16, %edx
7:
	/* The disk LBA: the run's, plus the offset in the drive's sectors.
	 * mull takes EDX, so the count goes on the stack across it. */
	pushl   %edx
	movzwl  %cs:VCD_OFF_PERCD, %edx
	mull    %edx
	movl    %eax, %ebp
	popl    %edx

	movl    8(%si), %eax
	addl    %ebp, %eax
	movl    %eax, %cs:(VCD_OFF_DAP + 8)
	movl    12(%si), %eax
	adcl    $0, %eax
	movl    %eax, %cs:(VCD_OFF_DAP + 12)

	/* Our packet: sixteen bytes, the caller's buffer, this pass's count
	 * in the drive's own sectors. */
	movw    $0x0010, %cs:VCD_OFF_DAP
	pushl   %eax
	pushl   %edx
	movl    %edx, %eax
	movzwl  %cs:VCD_OFF_PERCD, %edx
	mull    %edx
	movw    %ax, %cs:(VCD_OFF_DAP + 2)
	popl    %edx
	popl    %eax
	movw    %di, %cs:(VCD_OFF_DAP + 6)

	pushl   %ecx
	pushl   %ebx
	pushl   %edx
	pushw   %di

	pushw   %cs
	popw    %ds
	movw    $VCD_OFF_DAP, %si
	movb    %cs:VCD_OFF_PHYS, %dl
	movb    $0x42, %ah
	/* pushf then a far call, which is what an INT is: the handler's iret
	 * pops the flags we pushed, and a BIOS handler writes its result
	 * carry into that image exactly as this file's own tail does. */
	pushfw
	lcall   *%cs:VCD_OFF_OLDINT13

	popw    %di
	popl    %edx
	popl    %ebx
	popl    %ecx
	jc      read_fail

/* Both paths land here: EDX sectors have been transferred. A CD sector is
 * 128 paragraphs, so the buffer's SEGMENT moves rather than its offset, and
 * a read larger than 64 KB works. */
advance:
	addl    %edx, %ebx
	subl    %edx, %ecx
	pushl   %eax
	movl    %edx, %eax
	shlw    $7, %ax
	addw    %ax, %di
	movw    %di, %cs:(VCD_OFF_DAP + 6)
	popl    %eax
	testl   %ecx, %ecx
	jnz     read_loop

read_done:
	popw    %es
	popw    %ds
	popal
	xorb    %ah, %ah
	jmp     succeed

read_fail:
	popw    %es
	popw    %ds
	popal
	movb    $0x04, %ah        /* sector not found */
	jmp     fail

/* ---- returning ----
 *
 * An interrupt handler cannot just set the carry flag: iret restores the
 * flags the caller had. The flag image on the stack is the one that
 * survives, so that is the one written. After `pushw %bp` the frame is
 * [bp][ip][cs][flags], which puts the flags six bytes up. */
succeed:
	pushw   %bp
	movw    %sp, %bp
	andw    $0xFFFE, 6(%bp)
	popw    %bp
	iret

fail:
	pushw   %bp
	movw    %sp, %bp
	orw     $0x0001, 6(%bp)
	popw    %bp
	iret

vcd_blob_end:
