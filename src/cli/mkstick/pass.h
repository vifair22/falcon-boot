/* airies-boot mkstick: acquiring the passphrase that p3 and p4 share.
 *
 * Two routes, both ending in the same bytes: a file (for scripted builds and
 * for the test rig) or an interactive double prompt with the terminal echo
 * turned off.
 *
 * Input is restricted to printable ASCII, and that is a deliberate
 * compatibility rule rather than prudishness. This passphrase is typed again
 * at two very different prompts: the loader's own EFI console editor, which
 * reads UEFI scancodes, and a Linux initramfs prompt. There is no single
 * agreed byte encoding those two share outside ASCII, so a passphrase with a
 * non-ASCII character in it would be accepted here and then fail to unlock
 * p3 from the loader with no explanation available at that point. Refusing it
 * at creation time is the only place the user can still do something about it.
 * src/menu/prompt.c enforces the same rule from the other side.
 *
 * Lifetime: the bytes are wiped when the caller is done, and they never reach
 * a command line. cryptsetup gets them on stdin, which is why proc.h supports
 * a stdin buffer at all: an argv is world-readable through /proc while the
 * process runs.
 */

#ifndef AIRIES_MKSTICK_PASS_H
#define AIRIES_MKSTICK_PASS_H

#include <stddef.h>

#include <cli/common/status.h>

#define MKS_PASS_MAX 512u

typedef struct {
    char   buf[MKS_PASS_MAX];
    size_t len;
} mks_pass;

/* Reads from `file` when it is non-NULL (first line, trailing newline
 * stripped), otherwise prompts twice on the controlling terminal. */
cli_status pass_acquire(mks_pass *p, const char *file);

void pass_wipe(mks_pass *p);

#endif /* AIRIES_MKSTICK_PASS_H */
