/* falcon-boot: the firmware's TPM event log, replayed (docs/DESIGN.md
 * Section 13.15).
 *
 * Purpose. The station's TOTP seed is sealed to the PCR values the TPM holds
 * while the station's menu runs. Enrollment runs later, from the OS the
 * station started, and by then the PCRs have moved on: the station loaded a
 * UKI (PCR 4), shim logged the key that verified it (PCR 7), the kernel
 * measured its initrd and the firmware its ExitBootServices. So enrollment
 * cannot seal to what the PCRs hold; it replays the firmware's event log
 * instead and stops at the event that measured the station itself. What
 * the PCRs held just after that event is what they hold at the station's
 * menu, since the station measures nothing before it reads the seed.
 *
 * The station's event is found by its digest: the Authenticode hash of the
 * station image in the log's bank (trust/authenticode.h), which is what
 * shim and the firmware both record for an image they load. Finding it is
 * also the proof that the running system was started by this station, which
 * enrollment requires.
 *
 * Reasoning. Both log formats are read: a TPM 1.2's (TCG_PCR_EVENT, SHA-1
 * only) and a TPM 2.0's crypto-agile one (a Spec ID Event03 header, then
 * TCG_PCR_EVENT2 with a digest per bank). Fields are little-endian, the
 * platform's order. EV_NO_ACTION events are not extended, except that a
 * StartupLocality event sets PCR 0's starting value. PCRs 16 and up start
 * at other values and are not boot measurements, so a replay covers 0 to
 * 15 only.
 *
 * A replay that ran to the end must equal the TPM's PCRs now, or the log is
 * not the whole story (a truncated log, events outside it); enrollment
 * checks that before it trusts the stopped replay.
 *
 * Interactions. mkstick's `station totp enroll`, which hands over the log
 * from securityfs; crypto/sha1 and sha256 for the extends. EFI-free,
 * allocation-free.
 */

#ifndef FALCON_TPM_EVLOG_H
#define FALCON_TPM_EVLOG_H

#include <stdint.h>
#include <stddef.h>

#define EVLOG_PCRS       16u
#define EVLOG_DIGEST_MAX 32u

#define EVLOG_ALG_SHA1   0x0004u
#define EVLOG_ALG_SHA256 0x000Bu

/* The events enrollment cares about. */
#define EVLOG_EV_NO_ACTION       0x00000003u
#define EVLOG_EV_EFI_BOOT_APP    0x80000003u

enum {
    EVLOG_OK         = 0,
    EVLOG_ERR_ARG    = -1,
    EVLOG_ERR_FORMAT = -2,  /* an event runs past the end, or a bad header */
    EVLOG_ERR_BANK   = -3   /* the log does not carry the bank asked for */
};

typedef enum {
    EVLOG_FMT_NONE = 0,
    EVLOG_FMT_TCG12 = 1,    /* SHA-1 only */
    EVLOG_FMT_TCG2 = 2      /* crypto-agile */
} evlog_format;

typedef struct {
    evlog_format format;
    uint16_t     alg;
    size_t       digest_len;
    uint8_t      pcr[EVLOG_PCRS][EVLOG_DIGEST_MAX];
    size_t       events;     /* events read, including the stop event */
    int          stopped;    /* the stop event was found */
} evlog_replay;

/* Replays `log` in bank `alg` (EVLOG_ALG_SHA1 or EVLOG_ALG_SHA256; a 1.2
 * log carries SHA-1 alone). With stop_digest, the replay ends after the
 * first event in PCR stop_pcr whose digest in that bank is stop_digest,
 * and out->stopped says whether one was found; without, it runs to the
 * end. Trailing zero bytes end the log. */
int evlog_replay_run(const uint8_t *log, size_t len, uint16_t alg,
                     unsigned int stop_pcr, const uint8_t *stop_digest,
                     evlog_replay *out);

#endif /* FALCON_TPM_EVLOG_H */
