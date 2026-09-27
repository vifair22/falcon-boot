/* falcon-bios-mkident's argument rules. See mkidentargs.h. */

#include "mkidentargs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* A whole word as an unsigned number no larger than max: decimal, or hex
 * after 0x. Nothing may follow the digits. */
static int number(const char *s, uint64_t max, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || *s == '\0' || *s == '-' || *s == '+')
        return -1;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > max)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

int mkident_record(const char *role, const char *id_hex, const char *stage3,
                   const char *marker, ident_rec *out, const char **why)
{
    uint64_t s3 = 0;
    uint64_t mk = 0;

    if (role == NULL || id_hex == NULL || out == NULL || why == NULL)
        return -1;
    if (ident_from_strings(IDENT_VERSION, role, strlen(role), id_hex,
                           strlen(id_hex), out) != IDENT_OK) {
        *why = "the role must be boot, library or shelf, and the install "
               "ID 32 lowercase hex digits, not all zero";
        return -1;
    }
    if (number(stage3, 0xFFFFu, &s3) != 0 ||
        number(marker, 0xFFFFu, &mk) != 0) {
        *why = "the stage 3 and marker sectors must be numbers below 65536";
        return -1;
    }
    if (out->role != IDENT_ROLE_BOOT && (s3 != 0u || mk != 0u)) {
        *why = "only a boot record names a stage 3 or a marker sector";
        return -1;
    }
    out->stage3_sector = (uint16_t)s3;
    out->marker_sector = (uint16_t)mk;
    return 0;
}

int mkident_offset(const char *s, uint64_t *out)
{
    if (number(s, UINT64_MAX, out) != 0 || *out % 512u != 0u)
        return -1;
    return 0;
}
