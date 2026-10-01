/* falcon-bios stage 3: the header and the entry.
 *
 * WHY THERE IS A THIRD STAGE. Stage 2 is linked where real mode and protected
 * mode agree on what an address means, which puts a ceiling of about 33 KB on
 * it - not a budget anyone chose, but the distance from 0x7E00 to the end of
 * the first 64 KB segment. The loader proper is past that ceiling as soon as
 * it carries the filesystem registry, the ISO reader and a boot path, so it
 * is linked at a megabyte and loaded there once protected mode is up, where
 * the only limit is memory.
 *
 * What stays behind in stage 2 is everything that has to be reachable from
 * real mode: the thunk, and the gate that publishes it at a fixed address
 * (biosdefs.h, GATE_*). Stage 3 never drops below a megabyte itself; it calls
 * down through the gate, which is a jump instruction and four fields.
 *
 * The header is the first sixteen bytes, so stage 2 can read one sector,
 * learn the two lengths, read exactly the first and zero up to the second.
 * Two lengths because .bss is not in the image: stage 3 has buffers measured
 * in megabytes and no reason to carry them on disk.
 */

#include <start/biosdefs.h>

	.section .text.stage3hdr, "ax"
	.globl  stage3_header
stage3_header:
	.long   STAGE3_MAGIC
	.long   stage3_image_end - stage3_header   /* bytes to read */
	.long   stage3_bss_end - stage3_header     /* bytes to occupy */
	.long   0                    /* room to grow */

	.code32
	.globl  stage3_entry
/* Entered from stage 2 by an absolute call to STAGE3_LOAD + STAGE3_HDR, in
 * 32-bit protected mode, with the GDT and IDT stage 2 built still live.
 * Nothing here may assume a stack: stage 2's is in low memory and is about
 * to stop being the right one. */
stage3_entry:
	movl    $STAGE3_STACK, %esp
	movzbl  GATE_DRIVE, %eax
	subl    $12, %esp            /* 16-byte aligned at the call */
	pushl   %eax
	call    bios_main
1:	hlt
	jmp     1b
