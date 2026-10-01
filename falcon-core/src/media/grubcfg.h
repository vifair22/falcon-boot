/* falcon-boot: the grub.cfg the loader serves in place of an ISO's own
 * (DESIGN Section 6.4, D3b).
 *
 * With Secure Boot on, an ISO whose kernel shim will not run as a second
 * stage (no .sbat) still boots through its own chain: its MS-signed shim,
 * its vendor-signed GRUB, and a kernel GRUB verifies through shim. What GRUB
 * reads as its configuration is not verified by anyone, which is GRUB's own
 * design. So the loader serves the ISO with its grub.cfg's bytes replaced by
 * this one: the same kernel and initrds the ISO's default entry names, the
 * hooked cmdline, and falcon-hook's archive as one more initrd, read by GRUB
 * as a blocklist from past the ISO's end, where the virtual CD serves it.
 *
 * The replacement is exactly as long as the file it replaces (padded with a
 * comment), so no ISO9660 or UDF record changes: only the file's bytes do.
 */

#ifndef FALCON_GRUBCFG_H
#define FALCON_GRUBCFG_H

#include <stddef.h>
#include <stdint.h>

#include <media/bootcfg.h>

enum {
    GRUBCFG_OK        = 0,
    GRUBCFG_ERR_ARG   = -1,
    GRUBCFG_ERR_SPACE = -2   /* ours is longer than the file it replaces */
};

/* Writes the configuration into out, exactly size bytes (the length of the
 * file it replaces; cap at least that). e is the entry to boot, cmdline its
 * hooked cmdline, and the hook's archive sits at 512-byte sector
 * hook_sector of GRUB's $root device, hook_count sectors long. */
int grubcfg_write(const bootcfg_entry *e, const char *cmdline,
                  uint64_t hook_sector, uint64_t hook_count, size_t size,
                  char *out, size_t cap);

#endif /* FALCON_GRUBCFG_H */
