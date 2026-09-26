/* falcon jump's module link script (docs/DESIGN.md Section 14.4).
 *
 * Preprocessed, as the assembly is, so a module's slot comes from biosdefs.h
 * and is written in one place: the build defines JMOD_SLOT to the slot's
 * index and the address follows. One loadable section, the header first,
 * then the module's .bss outside the file; jmod.c checks the header's
 * image_end and mem_end, which are the two symbols below, against the file
 * the core read.
 */

#include <start/biosdefs.h>

SECTIONS
{
    . = JMOD_SLOT_ADDR(JMOD_SLOT);

    .image : {
        KEEP(*(.jmod.header))
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(8);
        __jmod_image_end = .;
    }

    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        __jmod_mem_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
