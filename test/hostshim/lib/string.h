/* Host shim for <lib/string.h>, used only by the unit tests.
 *
 * src/exfat/exfat.c includes <lib/string.h> for four mem* routines and the
 * unaligned little-endian loads. In the freestanding EFI build those mem*
 * routines come from src/lib/string.c, because there is no libc. On the host
 * there is a libc, and linking src/lib/string.c in would define memcpy and
 * friends a second time alongside glibc's. So the test build puts this
 * directory ahead of src/ on the include path: exfat.c's #include
 * <lib/string.h> lands here, the mem* declarations come from the real
 * <string.h>, and the get_le* helpers are repeated verbatim below.
 *
 * Keep the get_le* bodies byte-identical to src/lib/string.h. They are the
 * on-disk parsing primitives the driver's correctness rests on; a divergence
 * here would make the tests validate something the loader does not do.
 * The ucs2_* declarations are deliberately absent: the exFAT driver does not
 * use them, so nothing in the unit under test can reach them by accident.
 */

#ifndef FALCON_TEST_HOSTSHIM_LIB_STRING_H
#define FALCON_TEST_HOSTSHIM_LIB_STRING_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

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

#endif /* FALCON_TEST_HOSTSHIM_LIB_STRING_H */
