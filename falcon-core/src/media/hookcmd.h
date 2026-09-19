/* falcon-boot: the cmdline of a boot falcon-hook rebuilds the ISO for
 * (docs/DESIGN.md Section 6.4; docs/notes/coverage-roadmap.md L2).
 *
 * The ISO's own CD entry (falcon-core/src/media/bootcfg.c) already says how
 * the distro boots from its CD. falcon-hook recreates that CD as
 * /dev/mapper/falcon-iso, with the ISO's own label, so the entry's arguments
 * are right as they stand. What is added:
 *
 *   rdinit=/falcon/hook    the kernel starts the hook, not the distro's init;
 *                          an rdinit= the entry already had moves to
 *                          falcon.init=, where the hook finds what to exec
 *   a nudge per family     for the initramfs that does not look at a
 *                          device-mapper device on its own (roadmap L2 step
 *                          5): live-boot's scan skips dm-* by name, so a
 *                          boot=live entry gets live-media=/dev/mapper/
 *                          falcon-iso, which live-boot mounts directly
 *
 * Both go before a "--" in the entry: the kernel hands everything after one
 * to init.
 *
 * The one table of nudges is here so the loader (direct boot) and the sign
 * tool (a blessed wrap carrying the hook) cannot disagree about it.
 */

#ifndef FALCON_HOOKCMD_H
#define FALCON_HOOKCMD_H

#include <stddef.h>
#include <stdint.h>

enum {
    HOOKCMD_OK          = 0,
    HOOKCMD_ERR_BAD_ARG = -1,
    HOOKCMD_ERR_NOSPACE = -2
};

/* Where the hook sits in the initramfs, and the device it makes. */
#define HOOKCMD_HOOK_PATH  "/falcon/hook"
#define HOOKCMD_DEVICE     "/dev/mapper/falcon-iso"

/* The newc archive that carries the hook, to append after the entry's own
 * initrds (each padded to 4 bytes): /falcon, and /falcon/hook with the given
 * bytes, executable. hookcmd_archive_size is its length; hookcmd_archive
 * writes it into buf. HOOKCMD_ERR_NOSPACE when cap is short. */
size_t hookcmd_archive_size(size_t bin_len);
int hookcmd_archive(const void *bin, size_t bin_len, void *buf, size_t cap,
                    size_t *len);

/* Builds the hooked cmdline from base (the ISO entry's own arguments,
 * space-separated) into out. */
int hookcmd_build(const char *base, char *out, size_t cap);

#endif /* FALCON_HOOKCMD_H */
