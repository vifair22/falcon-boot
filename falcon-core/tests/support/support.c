/* Helpers every component's test binary links. */

#include "tests.h"

void ucs2_to_ascii(const uint16_t *src, char *dst, size_t cap)
{
    size_t i = 0;

    if (cap == 0)
        return;
    while (src[i] != 0 && i + 1 < cap) {
        dst[i] = (src[i] >= 0x20u && src[i] < 0x7Fu) ? (char)src[i] : '?';
        i++;
    }
    dst[i] = '\0';
}
