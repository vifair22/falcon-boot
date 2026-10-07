/* falcon-boot: the PCRs, and on a TPM 1.2 the firmware's event log, as
 * the UEFI station saves them (docs/DESIGN.md Section 12.15).
 *
 * Purpose. Enrollment seals the TOTP seed to the PCRs the station runs
 * with, which are not the ones a running Linux sees: by then a UKI has been
 * loaded. So the station reads PCRs 0 to 15 before it loads anything, on
 * every boot, and saves them in a volatile variable `station totp enroll`
 * reads through efivarfs.
 *
 * Version 1, TPM 1.2. On coreboot with an edk2 payload there are two event
 * logs, and Linux reads only coreboot's (PCR 2). edk2's, which holds the
 * station's own measurement and everything in PCRs 4, 5 and 7, is reachable
 * only through EFI_TCG_PROTOCOL, while boot services last, so the record
 * carries a copy of it, taken just before the PCRs (tcg/tcg.c). The PCR
 * values are what makes the saved log checkable: a replay of it to its end
 * must give them.
 *
 * Version 2, TPM 2.0: the SHA-256 bank and no log, since a TPM 2.0
 * firmware's log reaches Linux whole. The record is what lets enrollment
 * work on a firmware that extends PCRs it does not log (the AMD fTPM on an
 * ASUS B550), whose log no replay can turn into the station's PCRs.
 *
 * The record, little-endian:
 *
 *   0   "FEVL"
 *   4   u16 version (1 or 2)
 *   6   u16 PCR count (16)
 *   8   u16 digest length (20, SHA-1, in version 1; 32, SHA-256, in 2)
 *   10  u16 reserved, zero
 *   12  u32 log length (zero in version 2)
 *   16  the PCR values, count x length
 *   then the log, as the firmware holds it (version 1 only)
 *
 * The values are read from the TPM, but the variable is writable by root,
 * like every file enrollment reads; enrollment already trusts root, who
 * sees the seed it prints.
 *
 * Interactions. The UEFI station writes it (totp/totp.c through loader/),
 * mkstick's totpcore.c reads it. EFI-free, allocation-free.
 */

#ifndef FALCON_TPM_EVSAVE_H
#define FALCON_TPM_EVSAVE_H

#include <stddef.h>
#include <stdint.h>

#define EVSAVE_PCRS       16u
#define EVSAVE_DIGEST_LEN 20u           /* version 1, SHA-1 */
#define EVSAVE_DIGEST_LEN2 32u          /* version 2, SHA-256 */
#define EVSAVE_HDR        16u
#define EVSAVE_PCR_BYTES  ((size_t)EVSAVE_PCRS * EVSAVE_DIGEST_LEN)
#define EVSAVE_BODY       (EVSAVE_HDR + EVSAVE_PCR_BYTES)
#define EVSAVE_BODY2      (EVSAVE_HDR + (size_t)EVSAVE_PCRS * EVSAVE_DIGEST_LEN2)

/* The variable, under falcon's vendor GUID (loader/loadops.h). */
#define EVSAVE_VAR "FalconEvLog"

enum {
    EVSAVE_OK         = 0,
    EVSAVE_ERR_ARG    = -1,
    EVSAVE_ERR_SPACE  = -2,
    EVSAVE_ERR_FORMAT = -3   /* not a record of version 1 or 2 */
};

typedef struct {
    unsigned       version;  /* 1 or 2 */
    size_t         digest_len; /* EVSAVE_DIGEST_LEN or EVSAVE_DIGEST_LEN2 */
    const uint8_t *pcr;      /* EVSAVE_PCRS values, digest_len each */
    const uint8_t *log;      /* version 1 only; NULL and 0 in version 2 */
    size_t         log_len;
} evsave_view;

/* The record's size for a log of log_len bytes. */
size_t evsave_size(size_t log_len);

/* Writes a version 1 record into out; *out_len is its size. */
int evsave_write(uint8_t *out, size_t cap,
                 const uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN],
                 const uint8_t *log, size_t log_len, size_t *out_len);

/* Writes a version 2 record, EVSAVE_BODY2 bytes, into out. */
int evsave_write2(uint8_t *out, size_t cap,
                  const uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN2],
                  size_t *out_len);

/* Reads a record of either version; v points into buf. Bytes after the
 * log (version 1) or the PCRs (version 2) are refused. */
int evsave_read(const uint8_t *buf, size_t len, evsave_view *v);

#endif /* FALCON_TPM_EVSAVE_H */
