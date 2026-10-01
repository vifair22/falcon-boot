/* falcon-bios stage 1: the master boot record (docs/DESIGN.md Section
 * 3.3).
 *
 * 512 bytes. The BIOS loads this one sector to 0x7C00 in 16-bit real mode
 * with DL holding the drive it came from, and jumps to byte 0. Everything
 * this file does is get stage 2 into memory and jump to it; every decision
 * worth making is made in C, up there.
 *
 * gas rather than NASM: .S is preprocessed, so the load
 * address, the offsets and the serial port live in one header that the C
 * side can include too, rather than being written down twice.
 *
 * WHERE STAGE 2 IS. Not known here: the installer writes stage 2's first
 * LBA (LE32, at STAGE1_S2_LBA_OFF) and its real length in sectors (LE16, at
 * STAGE1_S2_SECTORS_OFF) into this sector, just in front of the disk
 * signature. LBA 34 on a GPT disk, LBA 1 on an MBR disk whose first
 * partition may start at 63, so the length is the real one and not a
 * reservation: reading a fixed 64 sectors from LBA 1 would read the first
 * partition's boot sector too. A sector nobody patched carries zero and
 * stops here with a message.
 *
 * HOW IT IS READ. INT 13h AH=42h when AH=41h reports the extensions: an LBA
 * out of a packet, no geometry. Otherwise by CHS, AH=02h, with the geometry
 * AH=08h reports, one call per track as boot_fd.S does, since a call across
 * a track boundary is refused by many BIOSes. Neither path can cross a
 * 64 KB boundary, which a DMA transfer cannot: stage 2 goes to 0x7E00 and
 * is at most STAGE2_SECTORS long, so it ends at 0xFE00 at the latest,
 * inside the first 64 KB, and the build refuses a longer one.
 *
 * The variables live in .text, written at run time: a .data section would
 * cost bytes in a sector that has none to spare, and this sector is only
 * ever run from memory.
 *
 * The drive number is NOT assumed. DL is whatever the BIOS booted from, and
 * on a USB stick that is rarely 0x80; it is saved here and handed to stage 2,
 * which hands it to the read callback.
 */

#include <start/biosdefs.h>

/* Fields of `dap` below, the read loop's state, as displacements from SI. */
#define DAP_CNT    2
#define DAP_OFF    4
#define DAP_LBA    8
#define DAP_SPT    16
#define DAP_HEADS  20

	.code16
	.section .text.stage1, "ax"
	.globl  _start
	.globl  s2_lba
	.globl  s2_sectors
	.globl  stage1_end

_start:
	/* The BIOS may enter with any segment setup and an unknown stack, so
	 * everything is established before a single push. cs is already 0
	 * because the jump target was 0x7C00 in segment 0. */
	cli
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $STAGE1_LOAD, %sp    /* stack grows down from under us */
	cld
	sti

	movb    %dl, boot_drive      /* whatever we were booted from */

	/* Serial first, so a failure can be seen on it: a BIOS with no video
	 * is common on the hardware this targets, one with no COM1 is not.
	 * Stage 1 says nothing when it works (stage 2's first line is the
	 * evidence it did); every byte of the sector's message room went to
	 * the CHS path. */
	call    serial_init

	/* Nothing written here: not installed, or installed by a tool that
	 * predates the fields. */
	movw    $msg_notstage2, %si
	cmpw    $0, s2_sectors
	je      fail

	/* The packet doubles as the read loop's state on both paths: where
	 * the next call starts (its LBA) and where it lands (its offset), with
	 * the geometry after it; SI stays on it so every field is a short
	 * displacement. s2_sectors counts down what is left. */
	movw    $dap, %si
	movl    s2_lba, %eax
	movl    %eax, DAP_LBA(%si)
	movw    $STAGE2_LOAD, DAP_OFF(%si)

	/* Extensions, or CHS. spt stays zero for the LBA path. */
	movb    $0x41, %ah
	movw    $0x55AA, %bx
	movb    boot_drive, %dl
	int     $0x13
	jc      1f
	cmpw    $0xAA55, %bx
	jne     1f
	testb   $1, %cl              /* the subset with AH=42h */
	jnz     read
1:	movb    $0x08, %ah
	movb    boot_drive, %dl
	xorw    %di, %di             /* ES:DI 0:0, as some BIOSes need */
	int     $0x13
	movw    $msg_readfail, %si
	jc      fail
	xorw    %ax, %ax
	movw    %ax, %es             /* AH=08h may hand back a table in ES */
	movw    $dap, %si
	andb    $0x3F, %cl
	movb    %cl, DAP_SPT(%si)
	movb    %dh, DAP_HEADS(%si)  /* the last head; one more is the count */
	incw    DAP_HEADS(%si)

	/* Three tries per call, with a drive reset between them. A USB stick
	 * behind a BIOS's mass-storage emulation can fail its first read while
	 * the emulation is still settling, and a reset is what the BIOS's own
	 * boot code does about it.
	 *
	 * By LBA the whole of stage 2 is one call. By CHS it is one sector a
	 * call: this path is a hard disk from before 1996 or a stick a BIOS
	 * presents as one, where a sector is milliseconds, and it spares the
	 * sector a track's arithmetic (a floppy, where it would cost a
	 * revolution a sector, boots through boot_fd.S instead). */
read:
	movw    $3, %bp
next:
	movw    s2_sectors, %ax
	cmpb    $0, DAP_SPT(%si)
	je      2f
	movb    $1, %al              /* AH is zero: at most STAGE2_SECTORS */
	movw    %ax, DAP_CNT(%si)
	movl    DAP_LBA(%si), %eax
	xorl    %edx, %edx
	divl    DAP_SPT(%si)         /* eax track, edx sector from 0 */
	incw    %dx                  /* AH=02h counts sectors from 1 */
	movb    %dl, %cl
	xorl    %edx, %edx
	divl    DAP_HEADS(%si)       /* eax cylinder, edx head */
	movb    %dl, %dh
	movb    %al, %ch             /* cylinder, low eight bits */
	shlb    $6, %ah
	orb     %ah, %cl             /* its top two, in CL's top two */
	movw    DAP_OFF(%si), %bx    /* ES:BX, ES zero */
	movw    $0x0201, %ax
	jmp     4f
2:	movw    %ax, DAP_CNT(%si)
	movb    $0x42, %ah
4:	movb    boot_drive, %dl
	int     $0x13
	jnc     5f
	xorb    %ah, %ah             /* AH=00h, reset the drive */
	movb    boot_drive, %dl
	int     $0x13
	decw    %bp
	jnz     next
	movw    $msg_readfail, %si
	jmp     fail

	/* One call's worth read: move the state on, and go again while any
	 * is left. */
5:	movzwl  DAP_CNT(%si), %eax
	addl    %eax, DAP_LBA(%si)
	shlw    $9, %ax
	addw    %ax, DAP_OFF(%si)
	movw    DAP_CNT(%si), %ax
	subw    %ax, s2_sectors
	jnz     read

	/* Something was read; make sure it is stage 2 before running it. */
	cmpl    $STAGE2_MAGIC, (GATE_MAGIC)
	movw    $msg_notstage2, %si
	jne     fail

	/* DL is the contract with stage 2: the drive it must read from. */
	movb    boot_drive, %dl
	ljmp    $0, $STAGE2_LOAD

/* si = the reason. On serial and on the screen, then a key, then the reset
 * vector. Never a halt: on a machine with no serial cable that is a hang. */
fail:
	call    puts
	movw    $msg_key, %si
	call    puts
	xorb    %ah, %ah
	int     $0x16                /* wait for a key */
	ljmp    $0xFFFF, $0x0000

/* si = NUL-terminated string, each character through INT 10h teletype and
 * then to COM1, polled: there is no interrupt handler yet at this point in
 * the boot. Clobbers ax, bx, cx, dx, si. */
puts:
	lodsb
	testb   %al, %al
	jz      2f
	pushw   %ax
	movb    $0x0E, %ah
	movw    $0x0007, %bx         /* page 0, light grey */
	int     $0x10
	popw    %bx                  /* the character, in bl */
	movw    $0xFFFF, %cx         /* SERIAL_SPINS, in 16 bits */
	movw    $(SERIAL_PORT + 5), %dx
1:	inb     %dx, %al
	testb   $0x20, %al           /* transmitter holding register empty */
	jnz     3f
	loop    1b
3:	movw    $SERIAL_PORT, %dx
	movb    %bl, %al
	outb    %al, %dx
	jmp     puts
2:	ret

/* ---- COM1 ----
 *
 * The setup is a table of port and value, low port byte first (every port is
 * SERIAL_PORT to SERIAL_PORT + 5, so the high byte is SERIAL_PORT's): six
 * writes as data is half the bytes of six writes as code.
 */
serial_init:
	movw    $serial_tab, %si
	movw    $6, %cx
	movb    $(SERIAL_PORT >> 8), %dh
1:	lodsb
	movb    %al, %dl
	lodsb
	outb    %al, %dx
	loop    1b
	ret

serial_tab:
	.byte   (SERIAL_PORT + 1) & 0xFF, 0x00  /* interrupts off */
	.byte   (SERIAL_PORT + 3) & 0xFF, 0x80  /* DLAB: the divisor next */
	.byte   SERIAL_PORT & 0xFF, SERIAL_DIVISOR
	.byte   (SERIAL_PORT + 1) & 0xFF, 0x00
	.byte   (SERIAL_PORT + 3) & 0xFF, 0x03  /* 8N1, DLAB back off */
	.byte   (SERIAL_PORT + 2) & 0xFF, 0xC7  /* FIFO on, cleared */

boot_drive:
	.byte   0

/* The disk address packet INT 13h AH=42h reads, then the CHS geometry
 * AH=08h gave, as 32-bit divisors: the read loop's whole state, reached as
 * short displacements from SI. spt stays zero on the LBA path, which is how
 * the loop tells the two apart. */
dap:
	.byte   16                   /* its size */
	.byte   0
	.word   0                    /* sectors, this call */
	.word   0                    /* offset */
	.word   0                    /* segment, always 0 */
	.quad   0                    /* LBA */
	.long   0                    /* sectors per track */
	.long   0                    /* heads */

msg_readfail:
	.asciz  "falcon-bios: read error\r\n"
msg_notstage2:
	.asciz  "falcon-bios: no stage 2\r\n"
msg_key:
	.asciz  "press a key\r\n"

stage1_end:
	/* Where the installer writes stage 2's place, inside the 440 bytes of
	 * boot code and just in front of the disk signature. gas refuses to
	 * move .org backwards, so if the code above ever reaches these bytes
	 * this line is where the build stops (and build.mk checks the symbols
	 * against biosdefs.h). Zero as built: a stage 1 nobody installed says
	 * so. */
	.org    _start + STAGE1_S2_LBA_OFF
s2_lba:
	.long   0
	.org    _start + STAGE1_S2_SECTORS_OFF
s2_sectors:
	.word   0

	/* Bytes 440-511, the disk signature, the partition table and 0x55AA,
	 * are the disk's: every installer writes the 440 bytes before them
	 * and nothing else. */
	.org    _start + 440

	/* The signature the BIOS looks for, at the last two bytes of the
	 * sector. The assertion is the point: if anything above grows past
	 * 510 bytes the build fails here rather than producing a sector that
	 * is not bootable. */
	. = _start + 510
	.byte   0x55, 0xAA
