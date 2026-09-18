/* falcon-boot: AES-XTS sector decryption (IEEE 1619), plain64 IV.
 *
 * Purpose. The mode LUKS2 encrypts p3 with, in both places the unlock
 * touches: the keyslot area, which holds the AF-split master key and is
 * always encrypted in 512-byte units, and the data segment, whose unit size
 * the header names ("sector_size", 512 or 4096 in the wild). DESIGN
 * Section 9 scopes the loader to decryption; the encrypt direction is built
 * anyway and is not called by the loader, because a decrypt-only
 * implementation can only ever be checked against itself, and the IEEE 1619
 * vectors and the round-trip tests are the strongest evidence available that
 * this matches what dm-crypt writes.
 *
 * Reasoning about the IV. "plain64" means the tweak is the 64-bit sector index,
 * little-endian, zero-padded to a block, and it is always counted in 512-byte
 * units: dm-crypt advances its IV by sector_size/512 per data unit unless the
 * volume carries the iv_large_sectors flag. Encoding that here would be wrong,
 * because the rule belongs to the LUKS segment and not to XTS, so this module
 * takes an already-computed sector index and falcon-core/src/luks owns the
 * arithmetic. Getting it wrong is invisible on a 512-byte volume and corrupts
 * every block but the first on a 4096-byte one.
 *
 * Reasoning about dispatch. Two implementations, chosen once at xts_setkey
 * time by crypto_has_aesni(): aesni.c on anything from 2010 onwards, the
 * portable aes.c elsewhere and in any host test run on a machine without the
 * instructions. Dispatching at key-schedule time rather than per sector
 * keeps the branch out of the inner loop and means a context carries exactly
 * one schedule pair, not both.
 *
 * Interactions. Built on aes.h, aesni.h and crypto.h; the only consumer is
 * falcon-core/src/luks.
 */

#ifndef FALCON_CRYPTO_XTS_H
#define FALCON_CRYPTO_XTS_H

#include <stdint.h>
#include <stddef.h>

#include <crypto/aes.h>
#include <crypto/aesni.h>

enum {
    XTS_IMPL_PORTABLE = 0,
    XTS_IMPL_AESNI    = 1
};

/* Key material; wipe with crypto_wipe when done (xts_wipe does it).
 *
 * The AES-NI arm carries two data schedules because AESENC and AESDEC do
 * not consume the same one: AESDEC needs the equivalent inverse cipher form
 * (reversed, InvMixColumns folded in). The portable arm needs only one
 * because aes.c's decrypt walks the forward schedule backwards. The loader
 * itself only ever decrypts; the extra 244 bytes buy an encrypt direction
 * that the IEEE 1619 vectors can check on the same code path the loader
 * runs, which is worth more than the space. */
typedef struct {
    uint8_t impl;
    union {
        struct { aes_key data, tweak; } p;
        struct { aesni_sched data_dec, data_enc, tweak; } n;
    } k;
} xts_ctx;

/* key is the concatenated XTS key: 32 bytes (two AES-128 halves) or 64
 * bytes (two AES-256 halves, which is what LUKS2 --key-size 512 gives).
 * The first half is the data key, the second the tweak key. */
int  xts_setkey(xts_ctx *c, const uint8_t *key, size_t key_len);

/* One data unit. len must be a nonzero multiple of 16. in and out may be
 * the same buffer. sector is the plain64 tweak value, already in the units
 * the volume uses (see the header comment). */
void xts_decrypt_sector(const xts_ctx *c, uint64_t sector,
                        const uint8_t *in, uint8_t *out, size_t len);
void xts_encrypt_sector(const xts_ctx *c, uint64_t sector,
                        const uint8_t *in, uint8_t *out, size_t len);

void xts_wipe(xts_ctx *c);

/* Which implementation a context selected; for the test suite, which runs
 * the vectors through both, and for a loader diagnostic line. */
int  xts_impl(const xts_ctx *c);
/* Forces the portable path regardless of CPUID. Test-suite hook: it is the
 * only way to exercise the fallback on a machine that has AES-NI, and every
 * machine this is developed on does. */
int  xts_setkey_portable(xts_ctx *c, const uint8_t *key, size_t key_len);

#endif /* FALCON_CRYPTO_XTS_H */
