/* falcon-boot mkstick: the pure half of `station totp` (docs/DESIGN.md
 * Section 12.15), split from totp.c so it runs under the host tests.
 *
 * What the enrollment computes rather than asks a device: the PCR list it
 * is given, the owner authorisation made from a file, the base32 secret and
 * the otpauth URI an authenticator reads (the Key Uri Format every app
 * takes: otpauth://totp/<issuer>:<account>?secret=...&issuer=...), and the
 * new STATION.INI, which is the old one with its [totp] section replaced
 * and everything else, comments included, left as it was. And the PCR
 * values the station runs with from the record it saved (tpm/evsave.h):
 * on a TPM 1.2 whose firmware keeps two logs, checked against the log the
 * station saved and the one Linux reads; on a TPM 2.0, checked against the
 * one Linux reads where that log accounts for the PCRs.
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

/* The station's saved log (tpm/evsave.h) as efivarfs names it: EVSAVE_VAR
 * under falcon's vendor GUID (falcon-efi/src/loader/loadops.c). efivarfs
 * puts the variable's four attribute bytes before its value. */
#define TOTPC_EVLOG_EFIVAR \
    "/sys/firmware/efi/efivars/FalconEvLog-50614485-081b-4b7c-8b54-2abeffdf2fb3"

enum {
    TOTPC_OK        = 0,
    TOTPC_ERR_ARG   = -1,
    TOTPC_ERR_SPACE = -2,
    TOTPC_ERR_INI   = -3,   /* the file there does not parse */
    TOTPC_ERR_LOG   = -4,   /* the saved record or a log does not parse */
    TOTPC_ERR_STATION = -5, /* the saved log has no measurement of it */
    TOTPC_ERR_PCR   = -6,   /* no replay gives the PCRs the station read */
    TOTPC_ERR_BOOT  = -7    /* the record is not of this boot */
};

/* Which logs gave the station's PCRs (totpc_merge12). */
enum {
    TOTPC_FROM_SAVED = 1,   /* the saved log alone: one firmware log */
    TOTPC_FROM_BOTH  = 2    /* Linux's log, then the saved one */
};

/* PCRs nothing extends between the station's menu and a running Linux: a
 * later load moves PCR 4 (and the authority behind it PCR 7), and
 * ExitBootServices PCR 5. The live check covers these (0-3, 6). */
#define TOTPC_STABLE_PCRS 0x4Fu

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

/* The PCRs at the station's menu on a TPM 1.2, from the record the station
 * saved (rec) and, when the firmware keeps a second log, the one Linux
 * publishes (fw; NULL or 0 when there is none). The saved log must hold
 * the station image's measurement in PCR 4 (station, its SHA-1
 * Authenticode digest), and a replay must give the PCRs the station read:
 * the saved log alone, else fw's then the saved log's. out is the station's
 * reading, *from says which replay matched it; on TOTPC_ERR_PCR, *bad_pcr
 * is the first PCR the last replay tried gets wrong. */
int totpc_merge12(const uint8_t *fw, size_t fw_len, const uint8_t *rec,
                  size_t rec_len, const uint8_t station[20],
                  uint8_t out[16][20], unsigned *from, unsigned *bad_pcr);

/* totpc_merge12, with `as` the SHA-1 Authenticode digest of a station image
 * not booted yet: the logs are checked as they are, then replayed again
 * with the station's measurement taken as `as`'s (evlog_replay_substitute),
 * and out is what the station will read once that image is the one
 * booted. mkstick update reseals to it before the reboot. `as` NULL is
 * totpc_merge12. */
int totpc_merge12_as(const uint8_t *fw, size_t fw_len, const uint8_t *rec,
                     size_t rec_len, const uint8_t station[20],
                     const uint8_t *as, uint8_t out[16][20], unsigned *from,
                     unsigned *bad_pcr);

/* The PCRs at the station's menu on a TPM 2.0, from the version 2 record
 * the station saved (rec) and the firmware's log as Linux reads it (log).
 * The log must hold the station image's measurement in PCR 4 (station, its
 * SHA-256 Authenticode digest). live, when given, is PCRs 0-15 as the TPM
 * holds them now: the record must equal it on each of TOTPC_STABLE_PCRS
 * the log shows nothing extending after the station's measurement
 * (TOTPC_ERR_BOOT otherwise; a UKI's systemd-stub extends PCR 1), and the
 * log is whole when a replay of it to
 * its end gives live on every PCR in mask and TOTPC_STABLE_PCRS. Without
 * live (the rig's swtpm, not the TPM that booted) the log is taken as
 * whole. A whole log, replayed up to the station's measurement, must give
 * the record's value on every PCR in mask (TOTPC_ERR_PCR otherwise); a log
 * that is not whole is set aside and the record stands alone, which is a
 * firmware that extends PCRs it does not log. out is the record's values,
 * *whole says which it was; on an error *bad_pcr is the PCR at fault. */
int totpc_station20(const uint8_t *rec, size_t rec_len, const uint8_t *log,
                    size_t log_len, const uint8_t station[32],
                    const uint8_t (*live)[32], uint32_t mask,
                    uint8_t out[16][32], int *whole, unsigned *bad_pcr);

/* STATION.INI with its [totp] replaced by t, or removed when t is NULL.
 * The text there must parse (TOTPC_ERR_INI with *err_line and the reason
 * in why, STATIONINI_WHY_MAX bytes), and so must the result. */
int totpc_ini_update(const char *text, size_t len, const stationini_totp *t,
                     char *out, size_t cap, size_t *out_len,
                     unsigned *err_line, char *why);

#endif /* FALCON_MKSTICK_TOTPCORE_H */
