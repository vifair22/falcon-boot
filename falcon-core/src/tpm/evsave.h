/* falcon-boot: the firmware's TPM 1.2 event log as the UEFI station saves
 * it (docs/DESIGN.md Section 13.15).
 *
 * Purpose. On coreboot with an edk2 payload and a TPM 1.2 there are two
 * event logs, and Linux reads only coreboot's (PCR 2). edk2's, which holds
 * the station's own measurement and everything in PCRs 4, 5 and 7, is
 * reachable only through EFI_TCG_PROTOCOL, while boot services last. So
 * the station saves it on every boot where that protocol is the TPM's
 * (tcg/tcg.c), in a volatile variable `station totp enroll` reads through
 * efivarfs, together with PCRs 0 to 15 as the TPM held them at the same
 * moment. Taken before the station loads anything, both are the state at
 * its menu, which is what enrollment seals to.
 *
 * The record, little-endian:
 *
 *   0   "FEVL"
 *   4   u16 version (1)
 *   6   u16 PCR count (16)
 *   8   u16 digest length (20, SHA-1)
 *   10  u16 reserved, zero
 *   12  u32 log length
 *   16  the PCR values, count x length
 *   336 the log, as the firmware holds it
 *
 * The PCR values are what makes the saved log checkable later: a replay of
 * the log to its end must give them, or the log is not the whole story.
 * They are read from the TPM, but the variable is writable by root, like
 * every file enrollment reads; enrollment already trusts root, who sees
 * the seed it prints.
 *
 * Interactions. The UEFI station writes it (totp/totp.c through loader/),
 * mkstick's totpcore.c reads it. EFI-free, allocation-free.
 */

#ifndef FALCON_TPM_EVSAVE_H
#define FALCON_TPM_EVSAVE_H

#include <stddef.h>
#include <stdint.h>

#define EVSAVE_PCRS       16u
#define EVSAVE_DIGEST_LEN 20u
#define EVSAVE_HDR        16u
#define EVSAVE_PCR_BYTES  ((size_t)EVSAVE_PCRS * EVSAVE_DIGEST_LEN)
#define EVSAVE_BODY       (EVSAVE_HDR + EVSAVE_PCR_BYTES)

/* The variable, under falcon's vendor GUID (loader/loadops.h). */
#define EVSAVE_VAR "FalconEvLog"

enum {
    EVSAVE_OK         = 0,
    EVSAVE_ERR_ARG    = -1,
    EVSAVE_ERR_SPACE  = -2,
    EVSAVE_ERR_FORMAT = -3   /* not a record, or not version 1 */
};

typedef struct {
    const uint8_t *pcr;      /* EVSAVE_PCRS values, EVSAVE_DIGEST_LEN each */
    const uint8_t *log;
    size_t         log_len;
} evsave_view;

/* The record's size for a log of log_len bytes. */
size_t evsave_size(size_t log_len);

/* Writes the record into out; *out_len is its size. */
int evsave_write(uint8_t *out, size_t cap,
                 const uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN],
                 const uint8_t *log, size_t log_len, size_t *out_len);

/* Reads a record; v points into buf. Bytes after the log are refused. */
int evsave_read(const uint8_t *buf, size_t len, evsave_view *v);

#endif /* FALCON_TPM_EVSAVE_H */
