/* falcon-bios link script (docs/notes/bios-route.md Section 5.3).
 *
 * Two images, linked separately, because they load at different addresses and
 * neither may contain a relocation: real-mode code is position-dependent and
 * there is no loader to fix anything up. Each section is placed at the
 * address biosdefs.h says the BIOS or stage 1 will put it at, so a symbol's
 * link-time address is its run-time address and a near jump is a near jump.
 *
 * The output is ELF and the build objcopies the one loadable section out of
 * it. Linking straight to binary would work too and would hide the section
 * layout from every tool that can read an ELF, which is not a trade worth
 * making for a file this small.
 */

/* Each link passes its own -e: stage 1 enters at _start, stage 2 at
 * stage2_start. The entry symbol does not reach the raw binary at all, since
 * both are entered by a jump to a fixed address, but leaving it unset makes
 * the linker warn on every stage 2 link. */

SECTIONS
{
    /* 0x7C00 for stage 1, 0x10000 for stage 2. The build passes -Ttext, so
     * this is only the default. */
    . = 0x7C00;

    .text : {
        *(.text.stage1)
        *(.text.stage2)
        *(.text.stage3hdr)
        *(.text*)
        *(.rodata*)
        *(.data*)

        /* The end of what objcopy carries, and what stage 3's header
         * declares its file length to be. */
        . = ALIGN(8);
        stage3_image_end = .;
    }

    /* .bss is NOT in the image.
     *
     * It was, at first, because there is no runtime to clear it: stage 1
     * reads stage 2 off the disk and jumps to it. But stage 3 has its own
     * loader - stage 2 - and megabytes of buffers it has no reason to carry
     * on disk, so stage 3's header declares a memory size as well as a file
     * size and stage 2 zeroes the difference.
     *
     * Stage 1 does no such thing, so stage 2 must have an empty .bss. The
     * build checks that rather than trusting it: a static added to a stage 2
     * source would otherwise start life holding whatever the BIOS left at
     * that address. */
    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        stage3_bss_end = .;
    }

    /* Nothing may reach the output but the one loadable section. A real-mode
     * image with a .bss the loader never zeroes, or a comment section that
     * pushes the 0x55AA signature past byte 510, fails in ways that look
     * like hardware faults. */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
