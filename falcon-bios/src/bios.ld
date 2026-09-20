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
    /* 0x7C00 for stage 1, overridden on the stage 2 link. The build passes
     * -Ttext, so this is the default rather than the decision. */
    . = 0x7C00;

    .text : {
        *(.text.stage1)
        *(.text.stage2)
        *(.text*)
        *(.rodata*)
        *(.data*)

        /* .bss goes in the image, zero-filled, rather than being left for a
         * runtime to clear. There is no runtime: stage 2 is read off the
         * disk by stage 1 and jumped to. Putting it inside this output
         * section rather than its own is what makes it PROGBITS, so the
         * zeros are in the file and `objcopy -j .text` carries them. The
         * cost is disk sectors, which STAGE2_SECTORS bounds and the build
         * checks. */
        *(.bss*)
        *(COMMON)
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
