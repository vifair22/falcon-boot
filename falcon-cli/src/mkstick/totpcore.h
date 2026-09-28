/* falcon-boot mkstick: the pure half of `station totp` (docs/DESIGN.md
 * Section 13.15), split from totp.c so it runs under the host tests.
 *
 * What the enrollment computes rather than asks a device: the PCR list it
 * is given, the owner authorisation made from a file, the base32 secret and
 * the otpauth URI an authenticator reads (the Key Uri Format every app
 * takes: otpauth://totp/<issuer>:<account>?secret=...&issuer=...), and the
 * new STATION.INI, which is the old one with its [totp] section replaced
 * and everything else, comments included, left as it was.
 */

#ifndef FALCON_MKSTICK_TOTPCORE_H
#define FALCON_MKSTICK_TOTPCORE_H

#include <stddef.h>
#include <stdint.h>

#include <media/stationini.h>

#define TOTP_ISSUER     "falcon-station"
#define TOTP_SEED_LEN   20u
#define TOTP_URI_MAX    512u
#define TOTP_PCRS_DEFAULT "0,2,4,7"

enum {
    TOTPC_OK        = 0,
    TOTPC_ERR_ARG   = -1,
    TOTPC_ERR_SPACE = -2,
    TOTPC_ERR_INI   = -3    /* the file there does not parse */
};

/* "0,2,4,7": PCR numbers 0 to 15, each once, spaces allowed. */
int totpc_parse_pcrs(const char *text, uint32_t *mask);

/* RFC 4648 base32, upper case, no padding (what authenticators take). */
int totpc_base32(const uint8_t *in, size_t n, char *out, size_t cap);

/* The otpauth URI for a seed; account is percent-encoded. */
int totpc_uri(const char *account, const uint8_t *seed, size_t n, char *out,
              size_t cap);

/* An owner authorisation file's bytes, a trailing newline (LF or CRLF)
 * dropped: what `echo secret > file` leaves. */
size_t totpc_trim(const uint8_t *p, size_t n);

/* The 1.2 owner secret from a file: exactly 20 bytes is the secret itself;
 * anything else is a passphrase, whose SHA-1 is the secret (TrouSerS'
 * tpm_takeownership convention). No file (p NULL) is the well-known 20
 * zero bytes. */
void totpc_owner12(const uint8_t *p, size_t n, uint8_t out[20]);

/* STATION.INI with its [totp] replaced by t, or removed when t is NULL.
 * The text there must parse (TOTPC_ERR_INI with *err_line and the reason
 * in why, STATIONINI_WHY_MAX bytes), and so must the result. */
int totpc_ini_update(const char *text, size_t len, const stationini_totp *t,
                     char *out, size_t cap, size_t *out_len,
                     unsigned *err_line, char *why);

#endif /* FALCON_MKSTICK_TOTPCORE_H */
