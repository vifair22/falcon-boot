/* falcon-boot host tooling: the memory a shelf's journal replay reads through
 * (falcon-core/src/fs/jbd2.h, fs_mount_journal), from malloc.
 *
 * A shelf the host reads may be one an OS has mounted right now, which is
 * the ordinary case for `mkstick station install` on the machine it
 * installs on: its superblock says the journal needs recovery for as long as
 * the OS has it mounted read-write. The tools read it as the loaders do,
 * through the journal and never writing.
 */

#ifndef FALCON_CLI_JMEM_H
#define FALCON_CLI_JMEM_H

#include <fs/fs.h>

/* One table is out at a time, as in the loaders: a get releases the one
 * before it, so a tool that mounts shelf after shelf into one static volume
 * holds only the last. */
extern const fs_jmem cli_jmem;

/* Releases the table, wiped first (its scratch block held plaintext from
 * the shelf). For a tool done with the volume; safe to call twice. */
void cli_jmem_release(void);

#endif /* FALCON_CLI_JMEM_H */
