/* falcon jump: reading a module off the floppy into its slot and starting
 * it (DESIGN Section 13.4). */

#ifndef FALCON_BIOS_JMODLOAD_H
#define FALCON_BIOS_JMODLOAD_H

#include <jump/jmod.h>
#include <fs/fs.h>

/* Reads `path` off `vol` into slot `slot_idx`, checks it (jmod_check),
 * zeroes its .bss and calls its init, which fills `ops`. 0 on success;
 * otherwise -1 with *why a sentence for the screen. */
/* Names what the modules are read off in jmod_load's sentences: "floppy"
 * (the default, jump's), "disk" (station-legacy's station partition). */
void jmod_set_medium(const char *noun);

int jmod_load(fs_vol *vol, const char *path, unsigned slot_idx,
              jmod_ops *ops, const char **why);

#endif /* FALCON_BIOS_JMODLOAD_H */
