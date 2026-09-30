/* falcon-bios: the memory a shelf's journal replay reads through. See
 * jnlmem.c.
 */

#ifndef FALCON_BIOS_JNLMEM_H
#define FALCON_BIOS_JNLMEM_H

#include <stdint.h>

#include <fs/fs.h>

/* The provider fs_mount_journal is handed: JNL_REGION, whole. One table is
 * out at a time, so a get gives up the one before it, and a volume mounted
 * through it is readable only until the next get. */
extern const fs_jmem jnl_mem;

/* Gives the table up, wiped. Safe to call twice. */
void jnl_wipe(void);

/* What a get could have: the region's length when this machine has it all
 * as RAM, else 0. What a refusal says was free. */
uint64_t jnl_free(void);

#endif /* FALCON_BIOS_JNLMEM_H */
