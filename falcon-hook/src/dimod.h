/* falcon-hook: a block driver the initramfs lacks, brought in from the ISO's
 * own package pool. See falcon-hook/src/dimod.c. */

#ifndef FALCON_HOOK_DIMOD_H
#define FALCON_HOOK_DIMOD_H

#include <stdint.h>

#include <media/extmap.h>

/* Reads the package named prefix<release>-di_*.udeb for the running kernel
 * out of the ISO (on the partition maj:min, base bytes into the device),
 * unpacks it with the initramfs's udpkg and loads module. 1 when the module
 * loaded; 0 when this initramfs has no udpkg, the ISO no such package, or
 * any step failed. */
int dimod_bring(const char *prefix, const char *module,
                const extmap_hdr *h, const uint8_t *map, uint32_t maj,
                uint32_t min, uint64_t base, char **envp);

#endif /* FALCON_HOOK_DIMOD_H */
