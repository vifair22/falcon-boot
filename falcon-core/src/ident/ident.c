/* The identity record's three encodings. See ident.h, and DESIGN Section
 * 3.1 for why the record rather than the partition table says what a
 * partition is.
 *
 * Binary layout, at IDENT_BIN_OFF of a boot sector:
 *      0  magic "FLCN"
 *      4  version
 *      5  role
 *      6  stage 3 sector, LE16
 *      8  marker sector, LE16
 *     10  reserved, 2 bytes, zero
 *     12  install ID, 16 bytes
 *     28  CRC-32, LE32, over bytes 0-27
 *
 * Every decoder is strict: a record is taken exactly as the writer wrote
 * it, or not at all. The record decides which partitions a loader will
 * touch, so a half-readable one is not something to make the best of.
 */

#include <ident/ident.h>
#include <lib/string.h>

#define BIN_MAGIC     0u
#define BIN_VERSION   4u
#define BIN_ROLE      5u
#define BIN_STAGE3    6u
#define BIN_MARKER    8u
#define BIN_RESERVED 10u
#define BIN_ID       12u
#define BIN_CRC      28u

#define ID_HEX_LEN (IDENT_ID_LEN * 2u)

static const char magic[4] = { 'F', 'L', 'C', 'N' };

/* CRC-32, gzip's (reflected 0xEDB88320, all-ones in and out), bitwise.
 *
 * Its own copy rather than a call to gz_crc32, deliberately. gz.c is
 * inflate with a CRC beside it, and no build here drops unreferenced code
 * (no --gc-sections outside falcon-hook), so a UEFI loader or a station
 * that reads a record would carry a whole decompressor for eight lines of
 * loop. The record is 28 bytes, so bit-at-a-time costs nothing, and like
 * gz_crc32 it keeps no table, which is what stage 2 needs (no .bss).
 * ident_test.c checks this against gz_crc32 so the two cannot drift. */
static uint32_t crc32_bits(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;

    while (n-- > 0u) {
        unsigned k;

        crc ^= *p++;
        for (k = 0; k < 8u; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int role_ok(int role)
{
    return role == IDENT_ROLE_BOOT || role == IDENT_ROLE_LIBRARY ||
           role == IDENT_ROLE_SHELF;
}

/* An all-zero install ID is a record nobody finished writing, and one that
 * would match every other unfinished one; no decoder accepts it. */
static int id_is_zero(const uint8_t *id)
{
    unsigned i;

    for (i = 0; i < IDENT_ID_LEN; i++) {
        if (id[i] != 0u)
            return 0;
    }
    return 1;
}

const char *ident_role_name(ident_role role)
{
    switch (role) {
    case IDENT_ROLE_BOOT:    return "boot";
    case IDENT_ROLE_LIBRARY: return "library";
    case IDENT_ROLE_SHELF:   return "shelf";
    default:                 return NULL;
    }
}

/* The role a name spells, exactly, or 0. */
static int role_of(const char *s, size_t len)
{
    int r;

    for (r = IDENT_ROLE_BOOT; r <= IDENT_ROLE_SHELF; r++) {
        const char *name = ident_role_name((ident_role)r);
        size_t n = 0;

        while (name[n] != '\0')
            n++;
        if (n == len && memcmp(s, name, n) == 0)
            return r;
    }
    return 0;
}

/* 32 lowercase hex digits into 16 bytes, or nonzero. Uppercase is refused:
 * the writer writes lowercase, and one spelling per ID is what lets a
 * person grep for it. */
static int id_from_hex(const char *s, size_t len, uint8_t *out)
{
    unsigned i;

    if (len != ID_HEX_LEN)
        return -1;
    for (i = 0; i < ID_HEX_LEN; i++) {
        char c = s[i];
        unsigned d;

        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a') + 10u;
        else
            return -1;
        if ((i & 1u) == 0u)
            out[i / 2u] = (uint8_t)(d << 4);
        else
            out[i / 2u] = (uint8_t)(out[i / 2u] | d);
    }
    return 0;
}

int ident_decode_bin(const uint8_t *sector, ident_rec *out)
{
    const uint8_t *b;
    ident_rec r;

    if (sector == NULL || out == NULL)
        return IDENT_ERR_ARG;
    b = sector + IDENT_BIN_OFF;
    if (memcmp(b + BIN_MAGIC, magic, sizeof(magic)) != 0)
        return IDENT_ERR_NONE;
    if (get_le32(b + BIN_CRC) != crc32_bits(b, BIN_CRC))
        return IDENT_ERR_BAD;
    if (b[BIN_VERSION] != IDENT_VERSION || !role_ok(b[BIN_ROLE]))
        return IDENT_ERR_BAD;
    if (b[BIN_RESERVED] != 0u || b[BIN_RESERVED + 1u] != 0u)
        return IDENT_ERR_BAD;

    r.version = b[BIN_VERSION];
    r.role = (ident_role)b[BIN_ROLE];
    r.stage3_sector = get_le16(b + BIN_STAGE3);
    r.marker_sector = get_le16(b + BIN_MARKER);
    memcpy(r.install_id, b + BIN_ID, IDENT_ID_LEN);
    /* The sector fields mean something on a boot partition only. */
    if (r.role != IDENT_ROLE_BOOT &&
        (r.stage3_sector != 0u || r.marker_sector != 0u))
        return IDENT_ERR_BAD;
    if (id_is_zero(r.install_id))
        return IDENT_ERR_BAD;
    *out = r;
    return IDENT_OK;
}

void ident_encode_bin(const ident_rec *r, uint8_t *sector)
{
    uint8_t *b;

    if (r == NULL || sector == NULL)
        return;
    b = sector + IDENT_BIN_OFF;
    memcpy(b + BIN_MAGIC, magic, sizeof(magic));
    b[BIN_VERSION] = r->version;
    b[BIN_ROLE] = (uint8_t)r->role;
    put_le16(b + BIN_STAGE3, r->stage3_sector);
    put_le16(b + BIN_MARKER, r->marker_sector);
    b[BIN_RESERVED] = 0;
    b[BIN_RESERVED + 1u] = 0;
    memcpy(b + BIN_ID, r->install_id, IDENT_ID_LEN);
    put_le32(b + BIN_CRC, crc32_bits(b, BIN_CRC));
}

int ident_from_strings(uint32_t version, const char *role, size_t role_len,
                       const char *id_hex, size_t id_len, ident_rec *out)
{
    ident_rec r;
    int rl;

    if (role == NULL || id_hex == NULL || out == NULL)
        return IDENT_ERR_ARG;
    if (version != IDENT_VERSION)
        return IDENT_ERR_BAD;
    rl = role_of(role, role_len);
    if (rl == 0)
        return IDENT_ERR_BAD;
    if (id_from_hex(id_hex, id_len, r.install_id) != 0 ||
        id_is_zero(r.install_id))
        return IDENT_ERR_BAD;
    r.version = (uint8_t)version;
    r.role = (ident_role)rl;
    r.stage3_sector = 0;
    r.marker_sector = 0;
    *out = r;
    return IDENT_OK;
}

/* "falcon <version> <role> <id>", single spaces, then at most one '\n'. */
int ident_decode_text(const char *buf, size_t len, ident_rec *out)
{
    static const char prefix[] = "falcon ";
    const size_t plen = sizeof(prefix) - 1u;
    uint32_t version = 0;
    size_t i, role_at, id_at;

    if (buf == NULL || out == NULL)
        return IDENT_ERR_ARG;
    if (len > 0u && buf[len - 1u] == '\n')
        len--;
    if (len < plen || memcmp(buf, prefix, plen) != 0)
        return IDENT_ERR_BAD;

    /* The version: decimal, no sign, no leading zero, no more than it can
     * hold. Only IDENT_VERSION is accepted below, but a later version is
     * read as a number so it is refused as itself. */
    i = plen;
    if (i >= len || buf[i] < '1' || buf[i] > '9')
        return IDENT_ERR_BAD;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        if (version > 255u)
            return IDENT_ERR_BAD;
        version = version * 10u + (uint32_t)(buf[i] - '0');
        i++;
    }
    if (i >= len || buf[i] != ' ')
        return IDENT_ERR_BAD;
    role_at = ++i;
    while (i < len && buf[i] != ' ')
        i++;
    if (i >= len)
        return IDENT_ERR_BAD;
    id_at = i + 1u;
    /* The ID runs to the end, so anything after it (a second space, a
     * '\r', a second line) fails its length or its digits. */
    return ident_from_strings(version, buf + role_at, i - role_at,
                              buf + id_at, len - id_at, out);
}

size_t ident_encode_text(const ident_rec *r, char *buf, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    const char *role;
    char tmp[IDENT_TEXT_MAX];
    size_t n = 0, k;
    unsigned i, v;

    if (r == NULL || buf == NULL)
        return 0;
    role = ident_role_name(r->role);
    if (role == NULL || r->version == 0u)
        return 0;

    memcpy(tmp, "falcon ", 7);
    n = 7;
    /* The version, 1 to 3 digits, most significant first. */
    v = r->version;
    if (v >= 100u)
        tmp[n++] = (char)('0' + v / 100u);
    if (v >= 10u)
        tmp[n++] = (char)('0' + (v / 10u) % 10u);
    tmp[n++] = (char)('0' + v % 10u);
    tmp[n++] = ' ';
    for (k = 0; role[k] != '\0'; k++)
        tmp[n++] = role[k];
    tmp[n++] = ' ';
    for (i = 0; i < IDENT_ID_LEN; i++) {
        tmp[n++] = hex[r->install_id[i] >> 4];
        tmp[n++] = hex[r->install_id[i] & 0xFu];
    }
    tmp[n++] = '\n';
    /* Longest is "falcon 255 library " (19) + 32 + '\n' = 52, well inside
     * IDENT_TEXT_MAX, so tmp never overflows; the caller's cap is checked
     * once the length is known. */
    if (n + 1u > cap)
        return 0;
    memcpy(buf, tmp, n);
    buf[n] = '\0';
    return n;
}

int ident_same_install(const ident_rec *a, const ident_rec *b)
{
    if (a == NULL || b == NULL || id_is_zero(a->install_id))
        return 0;
    return memcmp(a->install_id, b->install_id, IDENT_ID_LEN) == 0;
}
