/* falcon-boot mkstick: the MOK keypair and the signing calls.
 *
 * DESIGN Section 7.5 in code form. The public cert travels (p1, for
 * enrollment); the private key does not travel in the clear and lands only
 * inside p3's LUKS volume at mode 0600. Nothing in this file ever writes key
 * material to p1 or p2, and the loader is not involved in any of it: signing
 * is a private-key operation that happens here, in Linux userland, or in the
 * on-stick signing appliance, and nowhere else.
 *
 * The signing itself is `sbsign`, shelled out to. That is the intended
 * vendoring line of DESIGN Section 8: reimplementing Authenticode PE signing
 * in this repo would be madness for a config tool that runs on a full Linux
 * box, and a second implementation of a signature format is a second place to
 * get it subtly wrong.
 */

#ifndef FALCON_MKSTICK_KEYS_H
#define FALCON_MKSTICK_KEYS_H

#include <cli/common/status.h>
#include <cli/mkstick/disk.h>

typedef struct {
    char dir[MKS_PATH_MAX];
    char key[MKS_PATH_MAX];  /* PEM private key, mode 0600 */
    char crt[MKS_PATH_MAX];  /* PEM certificate, what sbsign takes */
    char cer[MKS_PATH_MAX];  /* DER certificate, what MokManager takes */
} mok_keys;

/* Default subject CN. Overridable so a user with several sticks can tell
 * their enrolled MOK entries apart in MokManager. */
#define MOK_DEFAULT_CN "falcon-boot MOK"

/* RSA-4096, ten years. Long-lived on purpose: a MOK entry is enrolled by hand
 * through a firmware UI, and an expiry that forces the user back through that
 * ceremony buys nothing when the same key still has to be trusted. */
#define MOK_DEFAULT_BITS "4096"
#define MOK_DEFAULT_DAYS "3650"

cli_status mok_paths(mok_keys *k, const char *dir);

/* Generates key + PEM cert + DER cert into k->dir, which must exist. Refuses
 * to overwrite an existing key: silently replacing one would orphan every
 * signature already made with it. */
cli_status mok_generate(const mok_keys *k, const char *cn);

/* sbsign in -> out. */
cli_status mok_sign(const mok_keys *k, const char *in, const char *out);

#endif /* FALCON_MKSTICK_KEYS_H */
