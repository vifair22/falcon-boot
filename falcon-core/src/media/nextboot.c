/* falcon-boot: the next-boot marker's firmware-free half. See nextboot.h.
 */

#include <media/nextboot.h>

#include <lib/string.h>

/* ---- the value ---- */

int nextboot_decode(const void *buf, size_t len, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t n;
    size_t i;

    if (out == (char *)0 || cap == 0)
        return NEXTBOOT_ERR_BAD_ARG;
    out[0] = '\0';
    if (p == (const unsigned char *)0 || len == 0)
        return NEXTBOOT_ERR_VALUE;

    /* One trailing NUL is what the writers put there, so the value reads as a
     * string in any variable dumper. A writer that omitted it is accepted for
     * the same reason: neither shape is ambiguous, and refusing one would be
     * a rule with no purpose behind it. */
    n = len;
    if (p[n - 1] == '\0')
        n--;
    if (n == 0 || n >= cap)
        return NEXTBOOT_ERR_VALUE;

    /* Judged whole before a byte of it is copied, so a refusal leaves the
     * caller's buffer empty rather than holding the prefix of something that
     * was rejected. Printable ASCII only, which is not a nicety: manifest_parse
     * drops any record carrying a byte outside it, so a value with one cannot
     * name a record and there is nothing to gain by carrying it further. An
     * embedded NUL lands here too and is refused rather than treated as a
     * terminator, so no two readers of this value can disagree about where it
     * ends. */
    for (i = 0; i < n; i++) {
        if (p[i] < 0x20u || p[i] > 0x7Eu)
            return NEXTBOOT_ERR_VALUE;
    }
    for (i = 0; i < n; i++)
        out[i] = (char)p[i];
    out[n] = '\0';
    return NEXTBOOT_OK;
}

int nextboot_names(const uint16_t *path, const char *name)
{
    char ascii[MANIFEST_PATH_MAX];

    if (path == (const uint16_t *)0 || name == (const char *)0 ||
        name[0] == '\0')
        return 0;
    if (manifest_path_from_ucs2(path, ascii, sizeof ascii) != MANIFEST_OK)
        return 0;
    return manifest_path_eq(ascii, name);
}

int nextboot_is_reserved(const char *name)
{
    return name != (const char *)0 && name[0] == ':';
}

int nextboot_is_personal(const char *name)
{
    const char *want = NEXTBOOT_PERSONAL;
    size_t i;

    if (name == (const char *)0)
        return 0;
    /* Exact match, not a prefix: ":personalish" is not this target, and
     * treating it as one would make the reserved namespace impossible to
     * extend without breaking older markers. */
    for (i = 0; want[i] != '\0'; i++) {
        if (name[i] != want[i])
            return 0;
    }
    return name[i] == '\0';
}

/* ---- the BIOS sector ---- */

/* The magic and the version as one line, which is what a marker this build
 * reads must begin with. */
static const char k_head[] = NEXTBOOT_MAGIC NEXTBOOT_VERSION "\n";

int nextboot_sector_is_marker(const uint8_t *sec, size_t len)
{
    static const char magic[] = NEXTBOOT_MAGIC;
    size_t i;

    if (sec == (const uint8_t *)0 || len < sizeof magic - 1u)
        return 0;
    for (i = 0; i + 1u < sizeof magic; i++) {
        if (sec[i] != (uint8_t)magic[i])
            return 0;
    }
    return 1;
}

/* The index of the first '\n' at or after `from`, or len when there is none
 * before the end. */
static size_t line_end(const uint8_t *sec, size_t from, size_t len)
{
    while (from < len && sec[from] != (uint8_t)'\n')
        from++;
    return from;
}

nextboot_sector nextboot_sector_parse(const uint8_t *sec, size_t len,
                                      uint64_t *stamp, char *out, size_t cap)
{
    uint64_t v = 0;
    size_t at;
    size_t end;
    size_t i;

    if (out != (char *)0 && cap != 0)
        out[0] = '\0';
    if (sec == (const uint8_t *)0 || len != NEXTBOOT_SECTOR_BYTES ||
        !nextboot_sector_is_marker(sec, len))
        return NEXTBOOT_SECTOR_NONE;
    if (stamp == (uint64_t *)0 || out == (char *)0 || cap == 0)
        return NEXTBOOT_SECTOR_BAD;

    /* The version, exactly. A later format is somebody else's to read; this
     * build clears it rather than guessing at it. */
    for (i = 0; i + 1u < sizeof k_head; i++) {
        if (sec[i] != (uint8_t)k_head[i])
            return NEXTBOOT_SECTOR_BAD;
    }
    at = sizeof k_head - 1u;

    /* The stamp: one to twenty digits, nothing else, no overflow. */
    end = line_end(sec, at, len);
    if (end == at || end == len || end - at > 20u)
        return NEXTBOOT_SECTOR_BAD;
    for (i = at; i < end; i++) {
        uint64_t d;

        if (sec[i] < (uint8_t)'0' || sec[i] > (uint8_t)'9')
            return NEXTBOOT_SECTOR_BAD;
        d = (uint64_t)(sec[i] - (uint8_t)'0');
        if (v > (~(uint64_t)0 - d) / 10u)
            return NEXTBOOT_SECTOR_BAD;
        v = v * 10u + d;
    }
    at = end + 1u;

    /* The value, by the same rule as the UEFI variable's bytes. */
    end = line_end(sec, at, len);
    if (end == len)
        return NEXTBOOT_SECTOR_BAD;
    if (nextboot_decode(sec + at, end - at, out, cap) != NEXTBOOT_OK)
        return NEXTBOOT_SECTOR_BAD;

    /* And nothing after it. A record with anything past its last line is not
     * one a writer of this format produced, and believing its first lines
     * would be believing half of something. */
    for (i = end + 1u; i < len; i++) {
        if (sec[i] != 0u) {
            out[0] = '\0';
            return NEXTBOOT_SECTOR_BAD;
        }
    }
    *stamp = v;
    return NEXTBOOT_SECTOR_VALUE;
}

int nextboot_sector_write(uint8_t *sec, size_t len, uint64_t stamp,
                          const char *value)
{
    char check[NEXTBOOT_MAX];
    char digits[20];
    size_t n = 0;
    size_t vlen = 0;
    size_t d = 0;
    size_t i;

    if (sec == (uint8_t *)0 || len != NEXTBOOT_SECTOR_BYTES ||
        value == (const char *)0)
        return NEXTBOOT_ERR_BAD_ARG;
    while (value[vlen] != '\0')
        vlen++;
    if (nextboot_decode(value, vlen, check, sizeof check) != NEXTBOOT_OK)
        return NEXTBOOT_ERR_VALUE;

    do {
        digits[d++] = (char)('0' + (int)(stamp % 10u));
        stamp /= 10u;
    } while (stamp != 0u);

    /* Fits by construction: the head, twenty digits and a value shorter than
     * NEXTBOOT_MAX are well under a sector. */
    memset(sec, 0, len);
    for (i = 0; i + 1u < sizeof k_head; i++)
        sec[n++] = (uint8_t)k_head[i];
    while (d > 0)
        sec[n++] = (uint8_t)digits[--d];
    sec[n++] = (uint8_t)'\n';
    for (i = 0; i < vlen; i++)
        sec[n++] = (uint8_t)value[i];
    sec[n] = (uint8_t)'\n';
    return NEXTBOOT_OK;
}

/* ---- dating it ---- */

nextboot_age nextboot_age_of(uint64_t stamp, uint64_t now, uint64_t window,
                             uint64_t skew)
{
    if (now < stamp)
        return stamp - now <= skew ? NEXTBOOT_AGE_FRESH : NEXTBOOT_AGE_FUTURE;
    if (now - stamp > window)
        return NEXTBOOT_AGE_STALE;
    return NEXTBOOT_AGE_FRESH;
}

/* A BCD byte as a number, or -1 when either nibble is not a decimal digit. */
static int bcd(uint8_t b)
{
    unsigned hi = (unsigned)(b >> 4);
    unsigned lo = (unsigned)(b & 0x0Fu);

    if (hi > 9u || lo > 9u)
        return -1;
    return (int)(hi * 10u + lo);
}

/* Days from 1970-01-01 to a proleptic Gregorian date: Howard Hinnant's
 * days_from_civil, restricted to years from 1970, where every intermediate
 * is non-negative and unsigned arithmetic is exact. */
static uint64_t days_from_civil(unsigned y, unsigned m, unsigned d)
{
    unsigned era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    if (m <= 2u)
        y--;
    era = y / 400u;
    yoe = y - era * 400u;
    doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (uint64_t)era * 146097u + doe - 719468u;
}

int nextboot_rtc_seconds(const uint8_t bcdv[7], uint64_t *out)
{
    static const uint8_t mdays[12] = { 31, 29, 31, 30, 31, 30,
                                       31, 31, 30, 31, 30, 31 };
    int f[7];
    unsigned year;
    size_t i;

    if (bcdv == (const uint8_t *)0 || out == (uint64_t *)0)
        return NEXTBOOT_ERR_BAD_ARG;
    for (i = 0; i < 7u; i++) {
        f[i] = bcd(bcdv[i]);
        if (f[i] < 0)
            return NEXTBOOT_ERR_VALUE;
    }
    year = (unsigned)f[0] * 100u + (unsigned)f[1];
    /* An hour with bit 7 set is a 12-hour-mode RTC's PM flag, and BCD()
     * already refused it; 24-hour ranges for the rest. February 29th is
     * accepted in any year: the RTC does its own calendar and a date it
     * reports is a date it counted to. */
    if (year < 1970u || f[2] < 1 || f[2] > 12 || f[3] < 1 ||
        f[3] > (int)mdays[f[2] - 1] || f[4] > 23 || f[5] > 59 || f[6] > 59)
        return NEXTBOOT_ERR_VALUE;

    *out = days_from_civil(year, (unsigned)f[2], (unsigned)f[3]) * 86400u +
           (uint64_t)f[4] * 3600u + (uint64_t)f[5] * 60u + (uint64_t)f[6];
    return NEXTBOOT_OK;
}
