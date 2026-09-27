/* falcon-boot appliance: which partitions are its stick's (docs/DESIGN.md
 * Section 3.1).
 *
 * The loader that started the appliance hands it the stick's install ID as
 * a credential (IDENT_CRED, the boot partition's record in its text form).
 * A partition is the stick's only when its own record carries that ID: p2
 * by the /.falcon file on its exFAT volume, p3 by the falcon token in its
 * LUKS2 header. There is no fallback: with no credential, or no partition
 * carrying the ID, the appliance unlocks nothing. A second falcon stick
 * plugged in beside this one carries another ID and is never touched.
 *
 * The judgement is here, apart from the /sys walk in stick.c, so the host
 * tests exercise it over synthetic volumes.
 */

#ifndef FALCON_APPLIANCE_STICKID_H
#define FALCON_APPLIANCE_STICKID_H

#include <stddef.h>

#include <fs/fs.h>
#include <ident/ident.h>

/* The install ID the credential carries: its bytes decoded as one text
 * record of role `boot`. IDENT_OK, or the decoder's refusal (IDENT_ERR_BAD
 * for a record of another role). */
int stickid_from_cred(const char *buf, size_t len, ident_rec *out);

/* The record a partition carries, read over `read`: the LUKS2 falcon token
 * when the volume starts with a LUKS2 header, else /.falcon on whatever
 * filesystem the registry recognises. IDENT_OK with *out, else the reason
 * there is none. */
int stickid_read(fs_read_fn read, void *ctx, ident_rec *out, int *luks);

typedef enum {
    STICKID_NONE = 0,   /* not this stick's, or not a partition it opens */
    STICKID_P2,         /* the stick's ISO library */
    STICKID_P3          /* the stick's LUKS2 shelf */
} stickid_take;

/* What a partition whose record is `rec` (LUKS2 when `luks`) is to the
 * stick `install` names. A plaintext shelf is NONE: it has no key on it,
 * and the appliance exists only on a stick whose p3 is encrypted. */
stickid_take stickid_judge(const ident_rec *install, const ident_rec *rec,
                           int luks);

#endif /* FALCON_APPLIANCE_STICKID_H */
