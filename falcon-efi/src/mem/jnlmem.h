/* falcon-efi: the memory a shelf's journal replay reads through. See
 * jnlmem.c.
 */

#ifndef FALCON_EFI_JNLMEM_H
#define FALCON_EFI_JNLMEM_H

#include <efi/uefi.h>
#include <fs/fs.h>

/* The provider fs_mount_journal is handed, bound to the firmware's boot
 * services. One table is out at a time, so a get gives up the one before
 * it, and a volume mounted through it is readable only until the next get
 * or jnl_release. */
void jnl_mem_make(fs_jmem *m, EFI_BOOT_SERVICES *bs);

/* Gives the table up, wiped. Safe to call twice. */
void jnl_release(void);

#endif /* FALCON_EFI_JNLMEM_H */
