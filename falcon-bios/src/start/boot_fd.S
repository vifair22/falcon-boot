/* falcon-bios stage 1 for a floppy: falcon jump's boot sector (docs/DESIGN.md
 * Section 13.3).
 *
 * The same job as boot.S, on a different medium: get stage 2 into memory at
 * STAGE2_LOAD and jump to it with DL naming the boot drive. Stage 2 is the
 * stick's, unchanged, at STAGE2_LBA onwards, in the FAT12 volume's reserved
 * area; so is stage 3, at FD_STAGE3_SECTOR, which stage 2 learns from the
 * identity record the build writes into this sector at IDENT_BIN_OFF
 * (0x1DC), after this code's end: stage 2 finds no partition table on a
 * floppy and reads the volume's own boot sector (DESIGN Section 3.3).
 * Nothing here knows FAT12. The volume's files are the core's business, in
 * C.
 *
 * WHY CHS HERE, when boot.S refuses it. A floppy is the one drive whose BIOS
 * support rarely answers INT 13h AH=41h/42h, so the extended read boot.S
 * relies on is not there to call. The geometry comes from this sector's own
 * BPB, which mformat writes for the medium the image was made for; nothing
 * is guessed.
 *
 * A TRACK AT A TIME. One AH=02h call per sector would cost a full revolution
 * each, 200 ms on a floppy, about thirteen seconds for stage 2 alone; one
 * call across a track boundary is refused by many controllers. So each call
 * reads to the end of the track it starts on. The destination never crosses
 * a 64 KB boundary (stage 2 ends at 0xFE00), which the floppy's DMA channel
 * cannot do either.
 */

#include <start/biosdefs.h>

/* The BPB fields this reads, as offsets into the sector. */
#define BPB_SPT     0x18
#define BPB_HEADS   0x1A

	.code16
	.section .text.stage1, "ax"
	.globl  _start
	.globl  fd_code_end

_start:
	jmp     start
	nop

	/* The BIOS parameter block. The values are a 1.44 MB floppy's and a
	 * placeholder reserved count; mformat -B keeps the code and rewrites
	 * every geometry field for the image it formats (apps/jump/app.mk),
	 * the reserved count included. */
	.ascii  "FALCONJP"           /* OEM name */
	.word   512                  /* bytes per sector */
	.byte   1                    /* sectors per cluster */
	.word   1                    /* reserved sectors, set by mformat -R */
	.byte   2                    /* FATs */
	.word   224                  /* root directory entries */
	.word   2880                 /* sectors */
	.byte   0xF0                 /* media descriptor */
	.word   9                    /* sectors per FAT */
	.word   18                   /* sectors per track */
	.word   2                    /* heads */
	.long   0                    /* hidden sectors */
	.long   0                    /* sectors, 32-bit */
	.byte   0                    /* drive number */
	.byte   0
	.byte   0x29                 /* extended boot signature */
	.long   0                    /* volume serial */
	.ascii  "FALCON JUMP"        /* volume label, 11 bytes */
	.ascii  "FAT12   "           /* filesystem type, 8 bytes */

start:
	cli
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $STAGE1_LOAD, %sp
	cld
	sti

	movb    %dl, boot_drive

	call    serial_init
	movw    $msg_stage1, %si
	call    serial_puts

	movw    $STAGE2_LBA, lba
	movw    s2_sectors, %ax          /* what the build says stage 2 is */
	movw    %ax, left
	movw    $STAGE2_LOAD, dest

next:
	/* lba / spt: the track, and the sector within it (zero-based). */
	movw    lba, %ax
	xorw    %dx, %dx
	divw    (STAGE1_LOAD + BPB_SPT)
	movb    %dl, %cl
	incb    %cl                  /* AH=02h counts sectors from 1 */
	/* What is left of this track, capped at what is left to read. */
	movw    (STAGE1_LOAD + BPB_SPT), %bx
	subw    %dx, %bx
	cmpw    left, %bx
	jbe     1f
	movw    left, %bx
1:	movw    %bx, count
	/* track / heads: the cylinder, and the head. */
	xorw    %dx, %dx
	divw    (STAGE1_LOAD + BPB_HEADS)
	movb    %al, %ch             /* cylinder, low eight bits */
	shlb    $6, %ah
	orb     %ah, %cl             /* its top two, in CL's top two */
	movb    %dl, %dh             /* head */
	movw    %cx, chs_cx
	movw    %dx, chs_dx

	/* Three tries, a reset between: a floppy's motor is still coming up
	 * to speed on the first. */
	movw    $3, %di
2:	movb    count, %al
	movb    $0x02, %ah
	movw    chs_cx, %cx
	movw    chs_dx, %dx
	movb    boot_drive, %dl
	movw    dest, %bx
	int     $0x13
	jnc     3f
	xorb    %ah, %ah
	movb    boot_drive, %dl
	int     $0x13
	decw    %di
	jnz     2b
	movw    $msg_readfail, %si
	jmp     fail

3:	movw    count, %ax
	addw    %ax, lba
	subw    %ax, left
	shlw    $9, %ax              /* sectors to bytes */
	addw    %ax, dest
	cmpw    $0, left
	jne     next

	cmpl    $STAGE2_MAGIC, (GATE_MAGIC)
	movw    $msg_notstage2, %si
	jne     fail

	movb    boot_drive, %dl
	ljmp    $0, $STAGE2_LOAD

/* si = the reason. On serial and on the screen, then a key, then the reset
 * vector, as boot.S. */
fail:
	call    serial_puts
	call    screen_puts
	movw    $msg_key, %si
	call    serial_puts
	call    screen_puts
	xorb    %ah, %ah
	int     $0x16
	ljmp    $0xFFFF, $0x0000

screen_puts:
	pushw   %si
1:	lodsb
	testb   %al, %al
	jz      2f
	movb    $0x0E, %ah
	movw    $0x0007, %bx
	int     $0x10
	jmp     1b
2:	popw    %si
	ret

serial_init:
	movw    $(SERIAL_PORT + 1), %dx
	movb    $0x00, %al
	outb    %al, %dx
	movw    $(SERIAL_PORT + 3), %dx
	movb    $0x80, %al
	outb    %al, %dx
	movw    $SERIAL_PORT, %dx
	movb    $SERIAL_DIVISOR, %al
	outb    %al, %dx
	movw    $(SERIAL_PORT + 1), %dx
	movb    $0x00, %al
	outb    %al, %dx
	movw    $(SERIAL_PORT + 3), %dx
	movb    $0x03, %al
	outb    %al, %dx
	movw    $(SERIAL_PORT + 2), %dx
	movb    $0xC7, %al
	outb    %al, %dx
	ret

/* si = NUL-terminated string. Clobbers ax, bx, cx, dx. */
serial_puts:
	pushw   %si
1:	lodsb
	testb   %al, %al
	jz      2f
	movb    %al, %bl
	movw    $0xFFFF, %cx
	movw    $(SERIAL_PORT + 5), %dx
3:	inb     %dx, %al
	testb   $0x20, %al
	jnz     4f
	loop    3b
4:	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	jmp     1b
2:	popw    %si
	ret

boot_drive:
	.byte   0
lba:
	.word   0
left:
	.word   0
dest:
	.word   0
count:
	.word   0
chs_cx:
	.word   0
chs_dx:
	.word   0

msg_stage1:
	.asciz  "falcon-bios: stage 1, floppy\r\n"
msg_readfail:
	.asciz  "falcon-bios: read error\r\n"
msg_notstage2:
	.asciz  "falcon-bios: no stage 2\r\n"
msg_key:
	.asciz  "press a key\r\n"

fd_code_end:
	/* The identity record goes at 0x1DC (falcon-core/src/ident/ident.h,
	 * IDENT_BIN_OFF), between here and the length word below; build.mk
	 * checks this code ends before it. */

	/* How many sectors stage 2 takes, which the build writes here once it
	 * knows (apps/jump/app.mk): stage 2 is about 19 sectors, and the 64 the
	 * stick reserves for it cost three more track reads on a floppy, each
	 * half a revolution of waiting. STAGE2_SECTORS until the build says. */
	. = _start + FD_S2_SECTORS_OFF
s2_sectors:
	.word   STAGE2_SECTORS

	. = _start + 510
	.byte   0x55, 0xAA
