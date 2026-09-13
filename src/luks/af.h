/* airies-boot: LUKS anti-forensic splitter, merge direction only.
 *
 * Purpose. A LUKS keyslot does not store the volume master key; it stores
 * the key expanded by the AF splitter into `stripes` blocks (4000 of them,
 * so 256 KB for a 64-byte key) and encrypts the whole expansion. Recovering
 * the master key means undoing that expansion, which is what this does.
 *
 * Reasoning for the design. The expansion exists so that overwriting a
 * keyslot has to destroy 256 KB rather than 64 bytes, which is a much
 * stronger guarantee on media that remaps writes. Undoing it is a strictly
 * sequential fold: the accumulator is XORed with each stripe in turn and
 * run through a hash-based diffusion after every one but the last. Because
 * it is sequential, this is an incremental API rather than a
 * merge-this-buffer call, and that is the whole point: src/luks/luks.c
 * decrypts the keyslot area one 512-byte XTS sector at a time and feeds the
 * stripes through as they appear, so the unlock never holds the 256 KB
 * expansion in memory. That turns a caller-supplied quarter-megabyte
 * scratch buffer into a 512-byte stack array, which matters in EFI-land
 * where the allocator is a firmware call and the stack is not large.
 *
 * The algorithm must match cryptsetup's lib/luks1/af.c byte for byte,
 * including the detail that each diffusion block is hashed as
 * BE32(block_index) || block_bytes and truncated to the block length. A
 * subtly different diffusion produces a master key that fails the digest
 * check, which is indistinguishable from a wrong passphrase, so this is
 * validated against real cryptsetup-created volumes rather than by
 * inspection.
 *
 * Interactions. Built on crypto/hash.h; used only by src/luks/luks.c.
 * The split direction is deliberately absent: the loader never writes a
 * keyslot (DESIGN Section 8 puts all volume creation in Linux userland).
 */

#ifndef AIRIES_LUKS_AF_H
#define AIRIES_LUKS_AF_H

#include <stdint.h>
#include <stddef.h>

#include <crypto/hash.h>

/* Bounded by the largest master key this loader handles: LUKS2
 * aes-xts-plain64 with --key-size 512 gives 64 bytes. */
#define AF_MAX_BLOCK 64u

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    AF_OK          = 0,
    AF_ERR_BAD_ARG = -1,
    AF_ERR_STATE   = -2   /* stripes fed out of step with the declared count */
};

typedef struct {
    const hash_desc *h;
    uint32_t blocksize;           /* bytes per stripe == master key length */
    uint32_t stripes;
    uint32_t seen;
    uint8_t  acc[AF_MAX_BLOCK];
} af_merge_ctx;

int af_merge_init(af_merge_ctx *c, const hash_desc *h,
                  uint32_t blocksize, uint32_t stripes);
/* Feed exactly `stripes` blocks of `blocksize` bytes, in order. */
int af_merge_stripe(af_merge_ctx *c, const uint8_t *stripe);
/* Writes blocksize bytes. Fails unless every stripe has been fed. */
int af_merge_final(af_merge_ctx *c, uint8_t *out);
void af_merge_wipe(af_merge_ctx *c);

#endif /* AIRIES_LUKS_AF_H */
