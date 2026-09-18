/* falcon-boot: AES-XTS on the AES-NI instruction set.
 *
 * Purpose. The fast path behind xts.h. Reading the personal-OS UKI off p3
 * means decrypting tens of megabytes through AES-XTS before the menu can
 * hand control to it, and the portable fallback in aes.c is roughly an order
 * of magnitude slower than the hardware instructions. DESIGN Section 9 calls
 * for AES-NI here by name.
 *
 * Reasoning for the file boundary. The intrinsics need -maes, and gcc is
 * entitled to use that instruction set anywhere in a translation unit
 * compiled with it. So this is a translation unit of its own, built with one
 * extra flag, and nothing else lives in it: not the CPUID probe (that is
 * crypto.c, built with the baseline flags, so it cannot fault on the very
 * CPUs it exists to detect) and not the dispatch (that is xts.c). The header
 * deliberately exposes plain C types only, so including it does not drag
 * <wmmintrin.h> into the baseline objects.
 *
 * Reasoning for the whole-sector entry points. XTS's tweak is sequential:
 * each 16-byte block's tweak is the previous one doubled in GF(2^128). A
 * per-block entry point would spill and reload that tweak on every call;
 * taking the whole sector keeps it in a register for all 32 (512-byte
 * sector) or 256 (4096-byte sector) blocks.
 *
 * Round keys are derived from falcon-core/src/crypto/aes.c's FIPS 197 key
 * schedule rather than from AESKEYGENASSIST. The expanded words are the same
 * object, and reusing the schedule the portable path already validates against
 * the standard's vectors removes an independent transcription that could be
 * subtly wrong only for one key size.
 *
 * Interactions. Called only by xts.c, and only when crypto_has_aesni()
 * returned nonzero.
 *
 * Note for the EFI build: the intrinsics assume the 16-byte stack alignment
 * that both the MS x64 ABI and the UEFI spec's calling convention require,
 * and that firmware has SSE enabled (UEFI requires MXCSR to be initialised,
 * so it does). Both hold on every platform this loader targets.
 */

#ifndef FALCON_CRYPTO_AESNI_H
#define FALCON_CRYPTO_AESNI_H

#include <stdint.h>
#include <stddef.h>

#define AESNI_MAX_ROUNDS 14u

/* Round keys as bytes rather than __m128i so this header stays free of
 * intrinsics; the implementation loads them unaligned. */
typedef struct {
    uint8_t  rk[(AESNI_MAX_ROUNDS + 1u) * 16u];
    uint32_t nr;
} aesni_sched;

/* key_len is 16 or 32. The decrypt schedule is the equivalent inverse
 * cipher form the AESDEC instruction expects: reversed round order with
 * InvMixColumns folded into the middle rounds. */
int aesni_setkey_enc(aesni_sched *s, const uint8_t *key, size_t key_len);
int aesni_setkey_dec(aesni_sched *s, const uint8_t *key, size_t key_len);

/* One XTS data unit. len must be a nonzero multiple of 16 (LUKS sector
 * sizes always are, so there is no ciphertext stealing here). tweak_key is
 * an ENCRYPT schedule in both directions: the tweak is always encrypted.
 * in and out may alias exactly. */
void aesni_xts_decrypt(const aesni_sched *data_key,
                       const aesni_sched *tweak_key, uint64_t sector,
                       const uint8_t *in, uint8_t *out, size_t len);
void aesni_xts_encrypt(const aesni_sched *data_key,
                       const aesni_sched *tweak_key, uint64_t sector,
                       const uint8_t *in, uint8_t *out, size_t len);

#endif /* FALCON_CRYPTO_AESNI_H */
