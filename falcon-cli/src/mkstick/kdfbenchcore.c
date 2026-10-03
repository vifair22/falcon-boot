/* falcon-boot mkstick: `kdf-bench`'s parsing and table. See kdfbenchcore.h. */

#include <mkstick/kdfbenchcore.h>

#include <stdio.h>
#include <string.h>

#include <crypto/argon2.h>
#include <crypto/crypto.h>

int kdfb_parse_list(const char *s, uint32_t lo, uint32_t hi, uint32_t *out,
                    size_t cap, size_t *n)
{
    *n = 0;
    if (s == NULL || *s == '\0')
        return KDFB_ERR_EMPTY;
    for (;;) {
        uint64_t v = 0;
        size_t digits = 0;

        while (*s >= '0' && *s <= '9') {
            v = v * 10u + (uint64_t)(*s - '0');
            if (v > 0xffffffffu)
                return KDFB_ERR_RANGE;
            s++;
            digits++;
        }
        if (digits == 0)
            return (*s == ',' || *s == '\0') ? KDFB_ERR_EMPTY : KDFB_ERR_NUM;
        if (*s != ',' && *s != '\0')
            return KDFB_ERR_NUM;
        if (v < lo || v > hi)
            return KDFB_ERR_RANGE;
        if (*n == cap)
            return KDFB_ERR_MANY;
        out[(*n)++] = (uint32_t)v;
        if (*s == '\0')
            return KDFB_OK;
        s++;
    }
}

int kdfb_parse_level(const char *s, int *level)
{
    static const struct {
        const char *name;
        int         level;
    } names[] = {
        { "auto",     ARGON2_SIMD_AUTO },
        { "portable", CRYPTO_SIMD_NONE },
        { "sse2",     CRYPTO_SIMD_SSE2 },
        { "ssse3",    CRYPTO_SIMD_SSSE3 },
        { "avx2",     CRYPTO_SIMD_AVX2 },
    };
    size_t i;

    for (i = 0; s != NULL && i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(s, names[i].name) == 0) {
            *level = names[i].level;
            return KDFB_OK;
        }
    }
    return KDFB_ERR_LEVEL;
}

/* Moves *at past what snprintf reported writing, held at the terminator
 * when buf was too short. */
static void advance(size_t *at, int w, size_t cap)
{
    if (w < 0)
        return;
    *at += (size_t)w;
    if (*at >= cap)
        *at = cap - 1u;
}

void kdfb_format_head(char *buf, size_t cap, const uint32_t *passes,
                      size_t n)
{
    size_t at = 0;
    size_t i;

    if (cap == 0)
        return;
    advance(&at, snprintf(buf, cap, "  memory"), cap);
    for (i = 0; i < n; i++)
        advance(&at, snprintf(buf + at, cap - at, "  t=%-5lu",
                              (unsigned long)passes[i]), cap);
}

void kdfb_format_row(char *buf, size_t cap, uint32_t mib, const double *secs,
                     size_t n)
{
    size_t at = 0;
    size_t i;

    if (cap == 0)
        return;
    advance(&at, snprintf(buf, cap, "%4lu MiB", (unsigned long)mib), cap);
    for (i = 0; i < n; i++) {
        if (secs[i] < 0.0)
            advance(&at, snprintf(buf + at, cap - at, "  %-7s", "-"), cap);
        else
            advance(&at, snprintf(buf + at, cap - at, "  %-7.2f", secs[i]),
                    cap);
    }
}
