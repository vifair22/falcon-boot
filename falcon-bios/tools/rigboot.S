/* falcon-bios station rig: a synthetic boot sector (docs/DESIGN.md Section
 * 12.11), what run-station-rig.sh installs in the rig disks' partitions and
 * MBRs in place of an operating system's, and run-cfstation-rig.sh on a
 * CF card.
 *
 * It reports, on COM1, what it was handed, as one line the rig judges:
 *
 *   RIGBOOT dl=<DL> dssi=<DS>:<SI> entry=<16 bytes at DS:SI>
 *     hidden=<dword at 0x7C1C> type=<byte 4 of the entry>
 *     sig80=<LBA 0 of drive 0x80, bytes 440-443> sigdl=<the same of DL>
 *     ndisks=<the BDA's disk count at 0x475> sig81=<the same of 0x81>
 *
 * dl, dssi and entry are the partition chain's contract (mbr/chain.c);
 * hidden is its BPB's hidden-sectors field as the station left it in
 * memory; sig80, sigdl and sig81 say which disk INT 13h answers for 0x80,
 * for DL and for 0x81, which is how the drive swap is seen from the
 * inside, and ndisks how many disks the booted system is told of (falcon
 * jump's CF route to a foreign disk, run-cfstation-rig.sh). Then it halts.
 *
 * One image serves as a partition's boot sector and as an MBR: a short jump
 * over the BPB (bytes 3-0x59, where FAT32's ends, filled from the volume
 * the rig splices it into), code from 0x5A, and a 20-byte tag at 0x1A4 the
 * rig writes a loader's name into ("NTLDR", "BOOTMGR", "WINBOOT SYS"), so
 * the station's autodetection lists it as that loader. As an MBR only bytes
 * 0-439 are written, so the disk's signature and table stay the disk's.
 *
 * 8086 real mode: the station hands over with CS 0 and IP 0x7C00.
 */

#define TAG_OFF  0x1A4
#define TAG_LEN  20

#define TAG_OFF  0x1A4
#define TAG_LEN  20
#define BUF      0x8000
/* What it saves, past the sector: the sector itself has no room. */
#define save_ds    0x7E00
#define save_si    0x7E02
#define save_dl    0x7E04
#define entry_copy 0x7E10
#define BDA_NDISKS 0x475

	.code16
	.section .text, "ax"
	.globl  _start
_start:
	.byte   0xEB, 0x58, 0x90         /* jmp short 0x5A; nop: FAT's own opening */
	.org    0x5A
start:
	cli
	xorw    %ax, %ax
	movw    %ax, %ss
	movw    $0x7C00, %sp
	sti
	/* DS, SI and DL as handed over, then the entry while DS is still the
	 * caller's. */
	movw    %ax, %es
	cld
	movw    $save_ds, %di
	movw    %ds, %ax
	stosw
	movw    %si, %ax
	stosw
	movb    %dl, %al
	stosb
	movw    $entry_copy, %di
	movw    $16, %cx
	rep movsb
	pushw   %es
	popw    %ds

	/* The labels are printed in the order they are laid out, so each
	 * `label` call prints the next one; BP walks them. */
	movw    $m_head, %bp
	call    label
	movb    save_dl, %al
	call    hex8
	call    label                    /* dssi */
	movw    save_ds, %ax
	call    hex16
	movb    $0x3A, %al
	call    putc
	movw    save_si, %ax
	call    hex16
	call    label                    /* entry */
	movw    $entry_copy, %si
	movw    $16, %cx
3:	lodsb
	call    hex8
	loop    3b
	call    label                    /* hidden */
	movw    0x7C1E, %ax
	call    hex16
	movw    0x7C1C, %ax
	call    hex16
	call    label                    /* type */
	movb    entry_copy + 4, %al
	call    hex8
	movb    $0x80, %dl               /* sig80 */
	call    sig
	movb    save_dl, %dl             /* sigdl */
	call    sig
	call    label                    /* ndisks */
	movb    BDA_NDISKS, %al
	call    hex8
	movb    $0x81, %dl               /* sig81 */
	call    sig
	call    label                    /* the line's end */
halt:
	cli
	hlt
	jmp     halt

/* The next label. */
label:
	movw    %bp, %si
	call    puts
	movw    %si, %bp
	ret

/* dl = drive: the next label, then its LBA 0, read by CHS, bytes 440-443
 * as the little-endian dword they are, or "--------". */
sig:
	call    label
	pushw   %bp
	movw    $0x0201, %ax
	movw    $0x0001, %cx
	xorb    %dh, %dh
	movw    $BUF, %bx
	int     $0x13
	popw    %bp
	jc      1f
	movw    BUF + 442, %ax
	call    hex16
	movw    BUF + 440, %ax
	jmp     hex16
1:	movw    $m_fail, %si
	jmp     puts

hex16:
	pushw   %ax
	movb    %ah, %al
	call    hex8
	popw    %ax
hex8:
	pushw   %ax
	shrb    $4, %al
	call    nib
	popw    %ax
nib:
	andb    $0x0F, %al
	addb    $'0', %al
	cmpb    $'9', %al
	jbe     putc
	addb    $('a' - '0' - 10), %al
putc:
	pushw   %dx
	pushw   %ax
	movw    $0x3FD, %dx
1:	inb     %dx, %al
	testb   $0x20, %al
	jz      1b
	popw    %ax
	movw    $0x3F8, %dx
	outb    %al, %dx
	popw    %dx
	ret

puts:
	lodsb
	testb   %al, %al
	jz      1f
	call    putc
	jmp     puts
1:	ret

/* In the order the report prints them. */
m_head:   .asciz "\r\nRIGBOOT dl="
m_dssi:   .asciz " dssi="
m_entry:  .asciz " entry="
m_hidden: .asciz " hidden="
m_type:   .asciz " type="
m_sig80:  .asciz " sig80="
m_sigdl:  .asciz " sigdl="
m_ndisks: .asciz " ndisks="
m_sig81:  .asciz " sig81="
m_end:    .asciz "\r\n"
m_fail:   .asciz "--------"

	.globl  rigboot_code_end
rigboot_code_end:

	.org    TAG_OFF
	.space  TAG_LEN
	.org    510
	.byte   0x55, 0xAA
