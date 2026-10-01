/* falcon jump: the header a module begins with, as the module declares it
 * (docs/DESIGN.md Section 13.4). jump/jmod.h has the same bytes as offsets,
 * which is how the core reads them; this is the i686 layout that puts them
 * there. The link script (jmod.ld.S) places the section first in the slot
 * and defines the two ends.
 */

#ifndef FALCON_BIOS_JMODHDR_H
#define FALCON_BIOS_JMODHDR_H

#include <jump/jmod.h>
#include <start/biosdefs.h>

typedef struct {
    uint32_t     magic;
    uint16_t     abi;
    uint16_t     hdr_len;
    const void  *slot;
    const void  *image_end;
    const void  *mem_end;
    jmod_init_fn init;
    char         name[8];
} jmod_header_decl;

_Static_assert(sizeof(jmod_header_decl) == JMOD_HDR_LEN,
               "the module header is jmod.h's 32 bytes");

extern char __jmod_image_end[];
extern char __jmod_mem_end[];

/* One line in the module's main file: its name, its slot index, its init. */
#define JMOD_HEADER(name_, slot_, init_)                                      \
    __attribute__((section(".jmod.header"), used))                           \
    static const jmod_header_decl jmod_header = {                                   \
        JMOD_MAGIC, JMOD_ABI, JMOD_HDR_LEN,                                  \
        (const void *)JMOD_SLOT_ADDR(slot_), __jmod_image_end,               \
        __jmod_mem_end, (init_), name_                                        \
    }

#endif /* FALCON_BIOS_JMODHDR_H */
