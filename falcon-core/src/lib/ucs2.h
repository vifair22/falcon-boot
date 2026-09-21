/* falcon-boot: UCS-2 string handling (UTF-16 without surrogates; filenames
 * only), shared by the exFAT driver's output, the UEFI console and the
 * volume survey.
 *
 * Split from <lib/string.h> because that header's other half is the four
 * freestanding mem* routines, which a hosted build must NOT link: glibc
 * already defines them. Keeping these here is what lets host-tested core
 * code handle a filename.
 */

#ifndef FALCON_LIB_UCS2_H
#define FALCON_LIB_UCS2_H

#include <stddef.h>
#include <stdint.h>

size_t ucs2_len(const uint16_t *s);
/* Copies at most cap-1 units and always terminates; returns units copied. */
size_t ucs2_copy(uint16_t *dst, size_t cap, const uint16_t *src);
/* ASCII-case-insensitive comparison, embedded-NUL-free strings. */
int ucs2_casecmp(const uint16_t *a, const uint16_t *b);
/* Does s end with suffix, ASCII-case-insensitively? */
int ucs2_ends_with(const uint16_t *s, const uint16_t *suffix);

#endif /* FALCON_LIB_UCS2_H */
