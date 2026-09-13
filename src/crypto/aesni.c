/* falcon-boot: AES-XTS on AES-NI. See aesni.h.
 *
 * BUILD REQUIREMENT: this translation unit alone needs -maes. The repo's
 * baseline CFLAGS must not carry it, because every other object would then
 * be free to use instructions that the CPUs this fallback exists for do not
 * have. See the Makefile's per-object rule for src/crypto/aesni.o.
 *
 * Only SSE2 is used beyond the AES instructions themselves (SSE2 is
 * architectural on x86-64), so -maes is the whole of the extra flag set.
 *
 * The GF(2^128) tweak doubling is the shift-and-reduce of IEEE 1619
 * Section 5.2: the tweak is a little-endian 128-bit value, doubling it is a
 * one-bit left shift, and a carry out of the top bit is reduced by the
 * polynomial x^128 + x^7 + x^2 + x + 1, i.e. XOR 0x87 into the lowest byte.
 * Done here with plain SSE2 shifts and a sign-broadcast mask rather than
 * PCLMULQDQ, which keeps the instruction-set requirement to AES-NI alone
 * (Westmere, 2010) instead of pulling in a second feature bit to probe.
 */

#include <crypto/aesni.h>
#include <crypto/aes.h>
#include <crypto/crypto.h>
#include <lib/string.h>

#include <wmmintrin.h>
#include <emmintrin.h>

/* Reads the FIPS 197 expanded key out of aes.c's schedule and lays each
 * round key down as the 16 bytes AESENC/AESDEC consume: word w[4r+c] is
 * column c, most significant byte first. */
static int expand(aesni_sched *s, const uint8_t *key, size_t key_len)
{
    aes_key k;
    uint32_t r, c;
    int rc;

    if (s == NULL || key == NULL) {
        return CRYPTO_ERR_BAD_ARG;
    }
    rc = aes_setkey(&k, key, key_len);
    if (rc != CRYPTO_OK) {
        return rc;
    }
    s->nr = k.nr;
    memset(s->rk, 0, sizeof s->rk);
    for (r = 0; r <= k.nr; r++) {
        for (c = 0; c < 4u; c++) {
            uint32_t w = k.rk[4u * r + c];
            uint8_t *p = s->rk + 16u * r + 4u * c;

            p[0] = (uint8_t)(w >> 24);
            p[1] = (uint8_t)(w >> 16);
            p[2] = (uint8_t)(w >> 8);
            p[3] = (uint8_t)w;
        }
    }
    crypto_wipe(&k, sizeof k);
    return CRYPTO_OK;
}

static __m128i rk_load(const aesni_sched *s, uint32_t r)
{
    return _mm_loadu_si128((const __m128i *)(const void *)(s->rk + 16u * r));
}

static void rk_store(aesni_sched *s, uint32_t r, __m128i v)
{
    _mm_storeu_si128((__m128i *)(void *)(s->rk + 16u * r), v);
}

int aesni_setkey_enc(aesni_sched *s, const uint8_t *key, size_t key_len)
{
    return expand(s, key, key_len);
}

int aesni_setkey_dec(aesni_sched *s, const uint8_t *key, size_t key_len)
{
    aesni_sched e;
    uint32_t r;
    int rc;

    rc = expand(&e, key, key_len);
    if (rc != CRYPTO_OK) {
        return rc;
    }
    /* Equivalent inverse cipher: reverse the round order and apply
     * InvMixColumns (AESIMC) to everything but the first and last. */
    s->nr = e.nr;
    memset(s->rk, 0, sizeof s->rk);
    rk_store(s, 0, rk_load(&e, e.nr));
    for (r = 1u; r < e.nr; r++) {
        rk_store(s, r, _mm_aesimc_si128(rk_load(&e, e.nr - r)));
    }
    rk_store(s, e.nr, rk_load(&e, 0));

    crypto_wipe(&e, sizeof e);
    return CRYPTO_OK;
}

static __m128i enc_block(const aesni_sched *k, __m128i s)
{
    uint32_t r;

    s = _mm_xor_si128(s, rk_load(k, 0));
    for (r = 1u; r < k->nr; r++) {
        s = _mm_aesenc_si128(s, rk_load(k, r));
    }
    return _mm_aesenclast_si128(s, rk_load(k, k->nr));
}

static __m128i dec_block(const aesni_sched *k, __m128i s)
{
    uint32_t r;

    s = _mm_xor_si128(s, rk_load(k, 0));
    for (r = 1u; r < k->nr; r++) {
        s = _mm_aesdec_si128(s, rk_load(k, r));
    }
    return _mm_aesdeclast_si128(s, rk_load(k, k->nr));
}

/* t <- t * x in GF(2^128), little-endian representation.
 *
 * The full 128-bit left shift is built from two 64-bit lane shifts: each
 * lane shifts in isolation, then the bit that fell off the low lane is
 * shifted across the lane boundary with a whole-register byte shift and
 * ORed into the high lane. The carry out of bit 127 is broadcast as a mask
 * by an arithmetic shift of the top dword and used to select the 0x87
 * reduction constant into the low lane, so the reduction is branch-free and
 * runs in constant time. */
static __m128i gf_double(__m128i t)
{
    __m128i sign = _mm_srai_epi32(_mm_shuffle_epi32(t, 0xff), 31);
    __m128i reduce = _mm_and_si128(sign, _mm_set_epi64x(0, 0x87));
    __m128i carry = _mm_slli_si128(_mm_srli_epi64(t, 63), 8);

    return _mm_xor_si128(_mm_or_si128(_mm_slli_epi64(t, 1), carry), reduce);
}

/* plain64: the IV is the 64-bit sector number little-endian, zero-padded to
 * the AES block, encrypted once under the tweak key. */
static __m128i tweak0(const aesni_sched *tweak_key, uint64_t sector)
{
    return enc_block(tweak_key, _mm_set_epi64x(0, (long long)sector));
}

void aesni_xts_decrypt(const aesni_sched *data_key,
                       const aesni_sched *tweak_key, uint64_t sector,
                       const uint8_t *in, uint8_t *out, size_t len)
{
    __m128i t = tweak0(tweak_key, sector);
    size_t i;

    for (i = 0; i + 16u <= len; i += 16u) {
        __m128i c = _mm_loadu_si128((const __m128i *)(const void *)(in + i));
        __m128i p = _mm_xor_si128(dec_block(data_key, _mm_xor_si128(c, t)), t);

        _mm_storeu_si128((__m128i *)(void *)(out + i), p);
        t = gf_double(t);
    }
}

void aesni_xts_encrypt(const aesni_sched *data_key,
                       const aesni_sched *tweak_key, uint64_t sector,
                       const uint8_t *in, uint8_t *out, size_t len)
{
    __m128i t = tweak0(tweak_key, sector);
    size_t i;

    for (i = 0; i + 16u <= len; i += 16u) {
        __m128i p = _mm_loadu_si128((const __m128i *)(const void *)(in + i));
        __m128i c = _mm_xor_si128(enc_block(data_key, _mm_xor_si128(p, t)), t);

        _mm_storeu_si128((__m128i *)(void *)(out + i), c);
        t = gf_double(t);
    }
}
