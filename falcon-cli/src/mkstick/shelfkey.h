/* falcon-boot mkstick: the MOK keypair off a shelf, for signing, without
 * the private key ever reaching a file (docs/DESIGN.md Section 7.5).
 *
 * The shelf is opened in this process with falcon-core's LUKS2 and ext4
 * (read through its journal when its OS has it mounted, fs/jbd2.h), and
 * the key and certificate go into two sealed memfds. sbsign is handed them
 * as /proc/self/fd/N, which it inherits: RAM, never a path on any disk,
 * gone when this process exits. The falcon-appliance's shelf.c does the
 * same for the sign tool.
 */

#ifndef FALCON_MKSTICK_SHELFKEY_H
#define FALCON_MKSTICK_SHELFKEY_H

#include <stdint.h>

#include <common/status.h>
#include <mkstick/keys.h>
#include <mkstick/pass.h>

typedef struct {
    int      key_fd;     /* -1 when closed */
    int      cert_fd;
    mok_keys k;          /* key and crt name the memfds; dir and cer empty */
} shelf_key;

/* The shelf at bytes [off, off + len) of disk: LUKS2 when luks, unlocked
 * with pass, else plaintext. CLI_ERR_PERM when the passphrase opens no
 * keyslot, CLI_ERR_NOTFOUND when the shelf has no keypair. */
cli_status shelf_key_open(shelf_key *s, const char *disk, uint64_t off,
                          uint64_t len, int luks, const mks_pass *pass);

/* Closes both memfds. Safe on one never opened, if zeroed or closed. */
void shelf_key_close(shelf_key *s);

#endif /* FALCON_MKSTICK_SHELFKEY_H */
