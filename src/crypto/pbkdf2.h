/* falcon-boot: PBKDF2 (RFC 8018 Section 5.2), HMAC PRF.
 *
 * Purpose. Two uses, both in the LUKS2 unlock of p3 (DESIGN Sections 3, 5):
 * deriving the keyslot key from the typed passphrase (the expensive one,
 * iterations pinned by mkstick at `--pbkdf-force-iterations`), and deriving
 * the digest that proves the recovered master key is the right one (cheap,
 * cryptsetup writes 1000-ish iterations there).
 *
 * Reasoning. p3 uses a pbkdf2 keyslot rather than LUKS2's default argon2id
 * precisely so this function is the entire cost of an unlock: argon2id is
 * memory-hard and brutal in a single-threaded EFI environment with no tuned
 * allocator, while PBKDF2-HMAC-SHA256 reduces to SHA-256 compressions that
 * a modern core chews through. The iteration count mkstick pins is budgeted
 * against the throughput this implementation actually achieves, so the
 * function is written to let hmac.h's precomputed pads do their job and
 * nothing else clever: no unrolling, no special-casing, spec-general dkLen.
 *
 * Interactions. Built on hmac.h; called from src/luks/luks.c for both the
 * keyslot KDF and the digest object, with the hash taken from the header in
 * each case rather than compiled in.
 */

#ifndef FALCON_CRYPTO_PBKDF2_H
#define FALCON_CRYPTO_PBKDF2_H

#include <stdint.h>
#include <stddef.h>

#include <crypto/hash.h>

/* Derives out_len bytes. iterations must be at least 1. out_len is bounded
 * only by the spec's (2^32-1)*hLen, which no caller here approaches.
 * Returns CRYPTO_OK or a negative CRYPTO_ERR_*. */
int pbkdf2(const hash_desc *d, const void *pass, size_t pass_len,
           const void *salt, size_t salt_len, uint32_t iterations,
           uint8_t *out, size_t out_len);

#endif /* FALCON_CRYPTO_PBKDF2_H */
