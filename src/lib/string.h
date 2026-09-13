/* falcon-boot: freestanding support routines.
 *
 * The four mem* functions exist because gcc is entitled to lower struct
 * copies and initializers into calls to them even under -ffreestanding; a
 * freestanding environment must provide them. The ucs2_* helpers cover the
 * UTF-16 string handling that both the exFAT driver's output and the UEFI
 * console input share.
 */

#ifndef FALCON_LIB_H
#define FALCON_LIB_H

#include <stdint.h>
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* Bounded UCS-2 (UTF-16 without surrogate handling; filenames only). */
size_t ucs2_len(const uint16_t *s);
/* Copies at most cap-1 chars and always terminates; returns chars copied. */
size_t ucs2_copy(uint16_t *dst, size_t cap, const uint16_t *src);
/* ASCII-case-insensitive comparison, embedded-NUL-free strings. */
int ucs2_casecmp(const uint16_t *a, const uint16_t *b);
/* Does s end with suffix, ASCII-case-insensitively? */
int ucs2_ends_with(const uint16_t *s, const uint16_t *suffix);

/* Unaligned little-endian loads for on-disk structure parsing. */
static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

#endif /* FALCON_LIB_H */
