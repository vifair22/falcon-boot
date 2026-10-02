/* falcon jump's rig: an option ROM that gives a PCI IDE controller in
 * native mode its I/O ports (docs/DESIGN.md Section 13.9, the cd-native
 * case).
 *
 * SeaBIOS maps every IDE-class device at the legacy ports whatever its
 * programming interface says (pciinit.c's storage_ide_setup, "as in ISA
 * mode"), and a controller that decodes its BARs then sits on the floppy
 * controller's 0x3F4 and over the PIIX's channels. SeaBIOS runs the option
 * ROMs after that and before it boots, so this one walks bus 0 for a class
 * 0x0101 function with a native bit in its programming interface and writes
 * its four BARs with I/O ports from 0xE000, where SeaBIOS's own allocation
 * (from 0xC000) does not reach, as a BIOS that knows native mode would
 * have. Says so on COM1 ("NATIDE: ...").
 *
 * 55 AA, one 512-byte unit, the init entry at offset 3 that SeaBIOS
 * far-calls in real mode; the build writes the last byte so the sum is
 * zero, which SeaBIOS checks before running it.
 */

	.code16
	.section .text, "ax"
	.globl  _start
_start:
	.byte   0x55, 0xAA
	.byte   1
init:
	pushal
	movw    $0xE000, %bp              /* the next controller's first port */
	xorl    %edi, %edi                /* dev << 11 */
dev_loop:
	movl    $0x80000000, %esi         /* bus 0, dev, fn 0, register 0 */
	orl     %edi, %esi
	movl    %esi, %eax
	call    cfg_read
	cmpw    $0xFFFF, %ax
	je      next_dev
	leal    0x0C(%esi), %eax
	call    cfg_read
	movb    $1, %cl                   /* functions to look at */
	testl   $0x00800000, %eax         /* a multifunction device */
	jz      fn_loop
	movb    $8, %cl
fn_loop:
	movl    %esi, %eax
	call    cfg_read
	cmpw    $0xFFFF, %ax
	je      next_fn
	leal    0x08(%esi), %eax
	call    cfg_read
	shrl    $8, %eax                  /* class, subclass, interface */
	movl    %eax, %ebx
	andl    $0xFFFF00, %eax
	cmpl    $0x010100, %eax
	jne     next_fn
	testb   $0x05, %bl                /* a channel in native mode */
	jz      next_fn
	call    assign
next_fn:
	addl    $0x100, %esi
	decb    %cl
	jnz     fn_loop
next_dev:
	addl    $0x800, %edi
	cmpl    $0x10000, %edi
	jb      dev_loop
	popal
	lret

/* The function at esi: BARs 0-3 at bp, bp+8, bp+0x10, bp+0x18, I/O space
 * decode on (the command register's status half is write-one-to-clear and
 * is masked off), and the line on COM1. */
assign:
	movzwl  %bp, %ebx
	orl     $1, %ebx
	leal    0x10(%esi), %eax
	call    cfg_write
	addl    $8, %ebx
	leal    0x14(%esi), %eax
	call    cfg_write
	addl    $8, %ebx
	leal    0x18(%esi), %eax
	call    cfg_write
	addl    $8, %ebx
	leal    0x1C(%esi), %eax
	call    cfg_write
	leal    0x04(%esi), %eax
	call    cfg_read
	andl    $0xFFFF, %eax
	orl     $1, %eax
	movl    %eax, %ebx
	leal    0x04(%esi), %eax
	call    cfg_write

	pushl   %esi
	movw    $(msg1 - _start), %si
	call    puts
	popl    %esi
	movl    %esi, %ebx
	shrl    $8, %ebx                  /* bus << 8 | dev << 3 | fn */
	call    puthex16
	pushl   %esi
	movw    $(msg2 - _start), %si
	call    puts
	popl    %esi
	movw    %bp, %bx
	call    puthex16
	pushl   %esi
	movw    $(msg3 - _start), %si
	call    puts
	popl    %esi
	addw    $0x20, %bp
	ret

cfg_read:                             /* eax: the address; eax out */
	movw    $0xCF8, %dx
	outl    %eax, %dx
	movw    $0xCFC, %dx
	inl     %dx, %eax
	ret

cfg_write:                            /* eax: the address, ebx: the value */
	movw    $0xCF8, %dx
	outl    %eax, %dx
	movw    $0xCFC, %dx
	movl    %ebx, %eax
	outl    %eax, %dx
	ret

puts:                                 /* si: the string, in this ROM */
1:	movb    %cs:(%si), %al
	incw    %si
	testb   %al, %al
	jz      2f
	call    putc
	jmp     1b
2:	ret

puthex16:                             /* bx: four digits */
	pushw   %cx
	movw    $4, %cx
1:	rolw    $4, %bx
	movb    %bl, %al
	andb    $0x0F, %al
	addb    $'0', %al
	cmpb    $'9', %al
	jbe     2f
	addb    $('a' - '9' - 1), %al
2:	call    putc
	loop    1b
	popw    %cx
	ret

putc:                                 /* al */
	movb    %al, %ah
	movw    $0x3FD, %dx
1:	inb     %dx, %al
	testb   $0x20, %al
	jz      1b
	movw    $0x3F8, %dx
	movb    %ah, %al
	outb    %al, %dx
	ret

msg1:	.asciz  "NATIDE: native IDE controller at bus/dev/fn 0x"
msg2:	.asciz  ", I/O BARs from 0x"
msg3:	.asciz  "\r\n"

	.org    511
	.byte   0                         /* the checksum, written by the build */
