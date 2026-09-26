/* falcon-bios: the resident INT 13h handler that serves an El Torito floppy
 * image as a memory disk (docs/notes/bios-route.md Sections 4.1 and 7.1).
 *
 * WHY THIS EXISTS. Some media boot a BIOS through a floppy-emulation El
 * Torito image (media type 1, 2 or 3) rather than a no-emulation CD: the
 * BIOS is meant to present the image as drive 0x00 and boot its first
 * sector. memtest86+ and Haiku do. falcon-bios answers it the way it answers
 * a CD, with a resident handler, but as a memory disk: the whole image (2.88
 * MB at most) is loaded into extended memory by vfd.c, and this serves reads
 * from it with INT 15h AH=87h, the one service that reaches past a megabyte.
 *
 * WHAT IT DECIDES: almost nothing. The geometry and the CHS-to-LBA
 * arithmetic are vfdcore.c's, host-tested; this reads a byte or two of state,
 * multiplies out an LBA, and asks the BIOS to move the bytes. Every field is
 * reached as a CS-relative numeric offset from biosdefs.h, and the block is
 * position independent, for the reasons vcdres.S's header gives.
 *
 * THE BLOCK (biosdefs.h has every offset):
 *   0x0000  the previous INT 13h vector, chained for a drive not ours
 *   0x0008  the drive we answer for, and the emulated geometry
 *   0x0014  the image's physical address in extended memory
 *   0x0020  scratch: an AH=87h source and destination and a read count
 *   0x0040  the descriptor table INT 15h AH=87h reads
 *   0x0070  the disk base table AH=08h hands back
 *   0x0080  the launch stub
 *   0x00C0  the handler
 */

#include <start/biosdefs.h>

	.code16
	.section .text.vfd, "ax"
	.globl  vfd_blob_start
	.globl  vfd_blob_end

vfd_blob_start:
	.org    VFD_OFF_OLDINT13
	.long   0
	.org    VFD_OFF_DRIVE
	.byte   0
	.org    VFD_OFF_HEADS
	.byte   0
	.org    VFD_OFF_SECTORS
	.byte   0
	.org    VFD_OFF_BTYPE
	.byte   0
	.org    VFD_OFF_CYLS
	.word   0
	.org    VFD_OFF_TOTAL
	.long   0
	.org    VFD_OFF_PHYS
	.long   0
	.org    VFD_OFF_STATUS
	.byte   0
	.org    VFD_OFF_SRC
	.long   0
	.org    VFD_OFF_DST
	.long   0
	.org    VFD_OFF_CNT
	.long   0
	.org    VFD_OFF_MOVEGDT
	.space  48
	.org    VFD_OFF_DBT
	.space  11
	.org    VFD_OFF_LDRIVE
	.byte   0

/* ---- the launch stub ----
 *
 * The last thing this loader runs. Reached through the gate (boot16), which
 * has already put the drive in DL. It copies the image's first sector, which
 * vfd.c staged in conventional memory, down to 0x7C00 and jumps to it as a
 * BIOS would jump to a booted floppy's boot sector.
 */
	.org    VFD_OFF_LAUNCH
	.globl  vfd_launch
vfd_launch:
	cli
	movw    $(VFD_STAGE_ADDR >> 4), %ax
	movw    %ax, %ds
	xorw    %si, %si
	xorw    %ax, %ax
	movw    %ax, %es
	movw    $0x7C00, %di
	movw    $512, %cx
	cld
	rep movsb

	/* DL is the contract: the boot sector reads the drive it came off it.
	 * boot16 set it already; set it again from our own copy so the stub is
	 * not coupled to that. */
	movb    %cs:VFD_OFF_LDRIVE, %dl
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $0x7C00, %sp
	sti
	ljmp    $0x0000, $0x7C00

	.org    VFD_OFF_CODE
/* ---- the handler ---- */
	.globl  vfd_entry
vfd_entry:
	/* Not our drive: on to whoever had the vector, as a jump so their iret
	 * returns to the original caller. */
	cmpb    %dl, %cs:VFD_OFF_DRIVE
	je      1f
	ljmp    *%cs:VFD_OFF_OLDINT13

1:	cmpb    $0x02, %ah
	je      vfd_read
	cmpb    $0x00, %ah
	je      vfd_ok            /* reset */
	cmpb    $0x0D, %ah
	je      vfd_ok            /* reset (hard-disk spelling; harmless) */
	cmpb    $0x01, %ah
	je      vfd_status
	cmpb    $0x04, %ah
	je      vfd_ok            /* verify */
	cmpb    $0x08, %ah
	je      vfd_params
	cmpb    $0x15, %ah
	je      vfd_type
	cmpb    $0x03, %ah
	je      vfd_wprot         /* write */
	cmpb    $0x05, %ah
	je      vfd_wprot         /* format */
	cmpb    $0x16, %ah
	je      vfd_ok            /* change-line status: no change */
	cmpb    $0x17, %ah
	je      vfd_ok            /* set media type for format */
	cmpb    $0x18, %ah
	je      vfd_ok
	/* Anything else: let the previous handler try. */
	ljmp    *%cs:VFD_OFF_OLDINT13

vfd_ok:
	movb    $0x00, %cs:VFD_OFF_STATUS
	xorb    %ah, %ah
	jmp     vfd_succeed

vfd_status:
	movb    %cs:VFD_OFF_STATUS, %ah
	testb   %ah, %ah
	jnz     vfd_fail
	jmp     vfd_succeed

vfd_wprot:
	movb    $0x03, %cs:VFD_OFF_STATUS
	movb    $0x03, %ah
	jmp     vfd_fail

/* AH=08h: the geometry. CH is the low eight bits of the maximum cylinder,
 * CL bits 0-5 the sectors per track (bits 6-7 the cylinder's top two, always
 * zero for an 80-cylinder floppy), DH the maximum head, DL the drive count,
 * BL the drive type, ES:DI a disk base table. */
vfd_params:
	movb    $0x00, %cs:VFD_OFF_STATUS
	movw    %cs:VFD_OFF_CYLS, %ax
	decw    %ax
	movb    %al, %ch
	movb    %cs:VFD_OFF_SECTORS, %cl
	movb    %cs:VFD_OFF_HEADS, %dh
	decb    %dh
	movb    $0x01, %dl
	movb    %cs:VFD_OFF_BTYPE, %bl
	movb    $0x00, %bh
	pushw   %cs
	popw    %es
	movw    $VFD_OFF_DBT, %di
	xorb    %ah, %ah
	xorb    %al, %al
	jmp     vfd_succeed

/* AH=15h: drive type. 01h is a floppy without change-line detection, which
 * spares this handler a change-line dance. */
vfd_type:
	movb    $0x00, %cs:VFD_OFF_STATUS
	movb    $0x01, %ah
	jmp     vfd_succeed

/* AH=02h: the read. CHS in CH/CL/DH, count in AL, buffer in ES:BX. The whole
 * image is a flat memory disk, so the emulated geometry is only how the LBA
 * is named; the bytes are one contiguous run and one AH=87h move serves any
 * count a floppy read can carry (a track at most, well under AH=87h's 64 KB).
 */
vfd_read:
	pushal
	movb    %al, %cs:VFD_OFF_CNT       /* count, while AL is still it */
	pushw   %ds
	pushw   %es

	/* The destination, ES:BX as a linear address. */
	movw    %es, %ax
	movzwl  %ax, %eax
	shll    $4, %eax
	movzwl  %bx, %ebx
	addl    %ebx, %eax
	movl    %eax, %cs:VFD_OFF_DST

	/* LBA = (cyl * heads + head) * sectors + (sector - 1). The cylinder's
	 * top two bits (CL 6-7) are zero for a floppy and are not read. */
	movzbl  %ch, %eax                  /* cylinder */
	movzbl  %cs:VFD_OFF_HEADS, %ebx
	imull   %ebx, %eax
	movzbl  %dh, %edx                  /* head */
	addl    %edx, %eax
	movzbl  %cs:VFD_OFF_SECTORS, %ebx
	imull   %ebx, %eax
	movzbl  %cl, %ebx
	andl    $0x3F, %ebx                /* sector, one-based */
	subl    $1, %ebx
	addl    %ebx, %eax                 /* eax = LBA */

	movzbl  %cs:VFD_OFF_CNT, %ecx      /* sectors wanted */
	/* Off the end of the medium is a real error; a zero count is a
	 * do-nothing success. */
	movl    %eax, %ebx
	addl    %ecx, %ebx
	cmpl    %cs:VFD_OFF_TOTAL, %ebx
	ja      vfd_read_fail
	testl   %ecx, %ecx
	jz      vfd_read_done

	/* The source, image_phys + LBA * 512. */
	shll    $9, %eax
	addl    %cs:VFD_OFF_PHYS, %eax
	movl    %eax, %cs:VFD_OFF_SRC

	shll    $9, %ecx                   /* bytes */

	/* The AH=87h descriptors: the source at MOVEGDT+16, the destination at
	 * +24, in the segment-descriptor shape (limit, base in three pieces,
	 * an access byte between them), exactly as vcdres.S builds them. */
	movl    %ecx, %ebp
	decl    %ebp
	movw    %bp, %cs:(VFD_OFF_MOVEGDT + 16)
	movw    %bp, %cs:(VFD_OFF_MOVEGDT + 24)
	movl    %cs:VFD_OFF_SRC, %eax
	movw    %ax, %cs:(VFD_OFF_MOVEGDT + 18)
	shrl    $16, %eax
	movb    %al, %cs:(VFD_OFF_MOVEGDT + 20)
	movb    %ah, %cs:(VFD_OFF_MOVEGDT + 23)
	movb    $0x93, %cs:(VFD_OFF_MOVEGDT + 21)
	movb    $0x00, %cs:(VFD_OFF_MOVEGDT + 22)
	movl    %cs:VFD_OFF_DST, %eax
	movw    %ax, %cs:(VFD_OFF_MOVEGDT + 26)
	shrl    $16, %eax
	movb    %al, %cs:(VFD_OFF_MOVEGDT + 28)
	movb    %ah, %cs:(VFD_OFF_MOVEGDT + 31)
	movb    $0x93, %cs:(VFD_OFF_MOVEGDT + 29)
	movb    $0x00, %cs:(VFD_OFF_MOVEGDT + 30)

	pushw   %es
	pushw   %si
	pushw   %cs
	popw    %es
	movw    $VFD_OFF_MOVEGDT, %si
	shrl    $1, %ecx                   /* words, which AH=87h counts */
	movb    $0x87, %ah
	int     $0x15
	popw    %si
	popw    %es
	jc      vfd_read_fail

vfd_read_done:
	movb    $0x00, %cs:VFD_OFF_STATUS
	popw    %es
	popw    %ds
	popal
	movzbl  %cs:VFD_OFF_CNT, %eax      /* AL = sectors read */
	xorb    %ah, %ah
	jmp     vfd_succeed

vfd_read_fail:
	movb    $0x04, %cs:VFD_OFF_STATUS
	popw    %es
	popw    %ds
	popal
	movb    $0x04, %ah                 /* sector not found */
	xorb    %al, %al
	jmp     vfd_fail

/* ---- returning ----
 *
 * The carry flag on the stack is the one iret restores, so that is the one
 * written. After `pushw %bp` the frame is [bp][ip][cs][flags]. */
vfd_succeed:
	pushw   %bp
	movw    %sp, %bp
	andw    $0xFFFE, 6(%bp)
	popw    %bp
	iret

vfd_fail:
	pushw   %bp
	movw    %sp, %bp
	orw     $0x0001, 6(%bp)
	popw    %bp
	iret

vfd_blob_end:
