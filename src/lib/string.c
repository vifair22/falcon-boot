#include <lib/string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a;
    const uint8_t *pb = b;
    for (; n--; pa++, pb++) {
        if (*pa != *pb)
            return (*pa < *pb) ? -1 : 1;
    }
    return 0;
}

size_t ucs2_len(const uint16_t *s)
{
    size_t n = 0;
    while (s[n] != 0)
        n++;
    return n;
}

size_t ucs2_copy(uint16_t *dst, size_t cap, const uint16_t *src)
{
    size_t n = 0;
    if (cap == 0)
        return 0;
    while (n + 1 < cap && src[n] != 0) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

static uint16_t ascii_lower(uint16_t c)
{
    if (c >= u'A' && c <= u'Z')
        return (uint16_t)(c + (u'a' - u'A'));
    return c;
}

int ucs2_casecmp(const uint16_t *a, const uint16_t *b)
{
    for (;; a++, b++) {
        uint16_t ca = ascii_lower(*a);
        uint16_t cb = ascii_lower(*b);
        if (ca != cb)
            return (ca < cb) ? -1 : 1;
        if (ca == 0)
            return 0;
    }
}

int ucs2_ends_with(const uint16_t *s, const uint16_t *suffix)
{
    size_t sl = ucs2_len(s);
    size_t xl = ucs2_len(suffix);
    if (xl > sl)
        return 0;
    return ucs2_casecmp(s + (sl - xl), suffix) == 0;
}
