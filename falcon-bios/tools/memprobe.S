/* falcon-bios rig fixture: a boot image that reports the memory map.
 *
 * A no-emulation El Torito boot image, chainloaded through the resident
 * virtual CD like any medium's own bootloader (`c` on its row). It asks the
 * questions a chainloaded bootmgr or kernel asks - INT 12h, INT 15h E820 in
 * full, E801 - and prints every answer to COM1, which is how the resident's
 * INT 15h half (DESIGN Section 11.3.8) is checked against what vcd.c meant
 * it to say. The resident is the one part of the tree no host test reaches,
 * so this is its test.
 *
 * One line per answer, fixed width, so a script can read it:
 *
 *   MEMPROBE INT12 0000027B
 *   MEMPROBE E820 0000000000000000 000000000009AC00 00000001
 *   MEMPROBE E801 3C00 3C00 7EF0 7EF0
 *   MEMPROBE DONE
 *
 * Then it halts. It is a fixture and the rig's timeout ends the run.
 */

#define COM1 0x3F8

	.code16
	.section .text, "ax"
	.globl  _start

_start:
	/* Loaded at 07C0:0000 by the resident's launch stub, so CS is 07C0
	 * and every label below is an offset from it. */
	cli
	movw    %cs, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $0xFFF0, %sp
	cld
	sti

	movw    $msg_int12, %si
	call    puts
	int     $0x12
	movzwl  %ax, %eax
	call    hex32
	call    crlf

	/* E820, every entry, into `ent` via ES:DI. */
	xorl    %ebx, %ebx
1:	movl    $0xE820, %eax
	movl    $0x534D4150, %edx
	movl    $24, %ecx
	movw    $ent, %di
	movl    $0, ent + 16
	int     $0x15
	jc      2f
	cmpl    $0x534D4150, %eax
	jne     2f
	pushl   %ebx
	movw    $msg_e820, %si
	call    puts
	movl    ent + 4, %eax
	call    hex32
	movl    ent, %eax
	call    hex32
	call    space
	movl    ent + 12, %eax
	call    hex32
	movl    ent + 8, %eax
	call    hex32
	call    space
	movl    ent + 16, %eax
	call    hex32
	call    crlf
	popl    %ebx
	testl   %ebx, %ebx
	jnz     1b
2:
	movw    $0xE801, %ax
	xorw    %bx, %bx
	xorw    %cx, %cx
	xorw    %dx, %dx
	int     $0x15
	jc      3f
	pushw   %dx
	pushw   %cx
	pushw   %bx
	pushw   %ax
	movw    $msg_e801, %si
	call    puts
	movw    $4, %cx
4:	popw    %ax
	pushw   %cx
	call    hex16
	call    space
	popw    %cx
	loop    4b
	call    crlf
3:
	movw    $msg_done, %si
	call    puts
5:	cli
	hlt
	jmp     5b

/* ---- COM1, polled, as boot.S does it ---- */

putc:	/* al */
	pushw   %dx
	pushw   %ax
	movw    $(COM1 + 5), %dx
1:	inb     %dx, %al
	testb   $0x20, %al
	jz      1b
	popw    %ax
	movw    $COM1, %dx
	outb    %al, %dx
	popw    %dx
	ret

puts:	/* si */
	lodsb
	testb   %al, %al
	jz      1f
	call    putc
	jmp     puts
1:	ret

space:
	movb    $' ', %al
	jmp     putc

crlf:
	movb    $'\r', %al
	call    putc
	movb    $'\n', %al
	jmp     putc

hex32:	/* eax, eight digits */
	movw    $8, %cx
1:	roll    $4, %eax
	pushl   %eax
	andb    $0x0F, %al
	addb    $'0', %al
	cmpb    $'9', %al
	jbe     2f
	addb    $('A' - '9' - 1), %al
2:	call    putc
	popl    %eax
	loop    1b
	ret

hex16:	/* ax, four digits */
	movw    $4, %cx
1:	rolw    $4, %ax
	pushw   %ax
	andb    $0x0F, %al
	addb    $'0', %al
	cmpb    $'9', %al
	jbe     2f
	addb    $('A' - '9' - 1), %al
2:	call    putc
	popw    %ax
	loop    1b
	ret

msg_int12: .asciz "MEMPROBE INT12 "
msg_e820:  .asciz "MEMPROBE E820 "
msg_e801:  .asciz "MEMPROBE E801 "
msg_done:  .asciz "MEMPROBE DONE\r\n"

	.balign 4
ent:	.space  24
