/* falcon-boot: Argon2id (RFC 9106, version 0x13).
 *
 * Purpose. The KDF of a LUKS2 keyslot made the ordinary way: `cryptsetup
 * luksFormat` defaults to argon2id, and a station's shelf is made by
 * cryptsetup on the machine (docs/notes/argon2-totp.md Section 1). luks.c
 * derives the keyslot key with this.
 *
 * Reasoning. The module never allocates. Argon2's cost is its memory, tens
 * of megabytes to a gigabyte, and where that comes from is the one thing the
 * three environments disagree on: the UEFI loader asks the firmware for
 * pages, falcon-bios takes a run of its E820 map, a host tool mallocs. So the
 * caller hands over the block, argon2_mem_kib says how big it must be, and
 * this file only fills it.
 *
 * Lanes are filled one after another, slice by slice: every lane's segment
 * of slice s before any lane's segment of slice s+1. That is the order the
 * specification's synchronisation points allow, since a segment only ever
 * references other lanes' blocks from slices already finished, so the result
 * is the one a parallel implementation with a thread per lane produces, only
 * slower. There are no threads in a loader to have.
 *
 * Stack. One 1 KiB block in argon2id and two in the segment fill, about
 * 3.5 KiB in all: the EFI build's 32 KiB frame limit and falcon-bios's
 * stage 3 stack both have room, and nothing here scales with the cost
 * parameters.
 *
 * Interactions. Built on blake2b.c; called from falcon-core/src/luks/luks.c.
 */

#ifndef FALCON_CRYPTO_ARGON2_H
#define FALCON_CRYPTO_ARGON2_H

#include <stdint.h>
#include <stddef.h>

#define ARGON2_BLOCK_BYTES 1024u
#define ARGON2_VERSION     0x13u
#define ARGON2_MIN_SALT    8u        /* the reference implementation's floor */
#define ARGON2_MIN_OUT     4u
#define ARGON2_MAX_LANES   0xffffffu

/* The inputs of RFC 9106 Section 3.1. secret (K) and ad (X) may be NULL
 * with a zero length; LUKS2 uses neither. m_cost is in KiB, one block each. */
typedef struct {
    const void *pwd;
    size_t      pwd_len;
    const void *salt;
    size_t      salt_len;
    const void *secret;
    size_t      secret_len;
    const void *ad;
    size_t      ad_len;
    uint32_t    t_cost;       /* passes, >= 1 */
    uint32_t    m_cost;       /* KiB, >= 8 * lanes */
    uint32_t    lanes;        /* parallelism p, 1..2^24-1 */
} argon2_params;

/* The memory argon2id fills, in KiB: m_cost rounded down to a multiple of
 * 4 * lanes (m' in the RFC). 0 when m_cost or lanes are out of range. */
uint32_t argon2_mem_kib(uint32_t m_cost, uint32_t lanes);

/* Derives out_len bytes (>= 4) into out.
 *
 * mem is the caller's, at least argon2_mem_kib(p->m_cost, p->lanes) KiB,
 * mem_kib says how much it is, and it must be 8-byte aligned. It is wiped
 * before return on every path that wrote to it, so the caller frees it as
 * it is.
 *
 * CRYPTO_OK; CRYPTO_ERR_BAD_ARG for parameters the RFC rules out;
 * CRYPTO_ERR_NOSPACE when mem_kib is short; CRYPTO_ERR_UNSUPPORTED when the
 * memory would not fit this build's address space. */
int argon2id(const argon2_params *p, void *mem, uint32_t mem_kib,
             uint8_t *out, size_t out_len);

#endif /* FALCON_CRYPTO_ARGON2_H */
