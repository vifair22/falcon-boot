/* Tests for falcon-core/src/part/part.c, the GPT parser behind partition
 * identification (docs/DESIGN.md Section 3).
 *
 * Two kinds of table are used, for two different reasons.
 *
 * Synthetic tables built byte by byte here reach every rejection path: sgdisk
 * will not write a header whose CRC is wrong, an entry size of 64, or an
 * entry array that claims to start inside the header. They also let the read
 * callback double as a fault injector, which is the only way to exercise the
 * I/O failure paths. The CRC-32 they are stamped with is computed by a plain
 * bit-at-a-time reference implementation in this file, deliberately not the
 * four-bit-table one in part.c: a table that agrees only with itself proves
 * nothing, and the reference is in turn pinned to the published IEEE 802.3
 * check value.
 *
 * One table written by sgdisk itself, on a plain sparse file with no loop
 * device and no sudo, is the evidence that the parser agrees with the
 * implementation that will actually stamp our sticks, partition types and p2's
 * name included. It skips cleanly when sgdisk is absent.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <part/part.h>

/* ---- reference CRC-32 (IEEE 802.3 / zlib, reflected 0xEDB88320) ---- */

static uint32_t ref_crc32(const uint8_t *p, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    unsigned b;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---- a disk in memory, with an optional read fault ---- */

#define TDISK_BLOCKS 80u

typedef struct {
    uint8_t *buf;
    uint64_t size;
    uint32_t bs;
    int      fail_on;   /* -1: never. Otherwise the 0-based call that fails. */
    int      calls;
} tdisk;

static int tdisk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    tdisk *d = ctx;
    int call = d->calls++;

    if (d->fail_on >= 0 && call >= d->fail_on)
        return -1;
    /* The parser's contract: block-aligned, whole blocks, inside the disk.
     * A violation is a parser bug, so it fails the test rather than the
     * read. */
    assert_int_equal(off % d->bs, 0);
    assert_int_equal(len % d->bs, 0);
    assert_true(off + len <= d->size);
    memcpy(buf, d->buf + off, (size_t)len);
    return 0;
}

static void tdisk_init(tdisk *d, uint32_t bs)
{
    d->bs = bs;
    d->size = (uint64_t)bs * TDISK_BLOCKS;
    d->buf = calloc(1, (size_t)d->size);
    assert_non_null(d->buf);
    d->fail_on = -1;
    d->calls = 0;
}

static void tdisk_free(tdisk *d)
{
    free(d->buf);
    d->buf = NULL;
}

/* ---- little-endian stores ---- */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

/* ---- synthetic GPT builder ---- */

/* Entry field offsets, spelt here rather than borrowed from part.c so the
 * test disagrees with the parser when the parser is wrong. */
#define TENT_NAME 56u

typedef struct {
    const uint8_t *type;      /* NULL leaves the entry unused */
    const char    *name;      /* ASCII, NUL-padded into the 36 units */
    uint64_t start;
    uint64_t end;
    uint32_t slot;            /* 0-based index in the entry array */
} tpart;

/* Layout knobs the rejection tests need to bend one at a time. */
typedef struct {
    uint32_t hdr_size;
    uint32_t ent_size;
    uint32_t ent_count;
    uint64_t ent_lba;
    uint64_t my_lba;
} tgeom;

static void geom_default(tgeom *g)
{
    g->hdr_size = 92;
    g->ent_size = 128;
    g->ent_count = 128;
    g->ent_lba = 2;
    g->my_lba = 1;
}

/* Writes a primary GPT at LBA 1 with its entry array at g->ent_lba, and
 * stamps both CRCs so the result is a table the parser must accept. */
static void build_gpt(tdisk *d, const tgeom *g, const tpart *parts, unsigned n)
{
    uint8_t *hdr = d->buf + d->bs;
    uint8_t *arr = d->buf + g->ent_lba * d->bs;
    uint64_t array_bytes = (uint64_t)g->ent_count * g->ent_size;
    unsigned i;

    assert_true(g->ent_lba * d->bs + array_bytes <= d->size);
    memset(hdr, 0, d->bs);
    memset(arr, 0, (size_t)array_bytes);

    for (i = 0; i < n; i++) {
        uint8_t *e = arr + (uint64_t)parts[i].slot * g->ent_size;
        unsigned k;

        if (parts[i].type == NULL)
            continue;
        memcpy(e, parts[i].type, GPT_GUID_LEN);
        /* A unique GUID that encodes its own slot, so a test can tell which
         * entry it got back without a second table of expectations. */
        for (k = 0; k < GPT_GUID_LEN; k++)
            e[16 + k] = (uint8_t)(0xA0u + parts[i].slot * 16u + k);
        put_le64(e + 32, parts[i].start);
        put_le64(e + 40, parts[i].end);
        /* NUL padding is what the spec says and what sgdisk writes, so the
         * builder writes it too: the array was memset above and only the
         * units the name occupies are filled. */
        for (k = 0; parts[i].name != NULL && k < GPT_NAME_LEN &&
                    parts[i].name[k] != '\0'; k++)
            put_le16(e + TENT_NAME + k * 2u,
                     (uint16_t)(unsigned char)parts[i].name[k]);
    }

    memcpy(hdr, "EFI PART", 8);
    put_le32(hdr + 8, 0x00010000u);          /* revision 1.0 */
    put_le32(hdr + 12, g->hdr_size);
    put_le64(hdr + 24, g->my_lba);
    put_le64(hdr + 32, TDISK_BLOCKS - 1u);   /* alternate (backup) header */
    put_le64(hdr + 40, 34);                  /* first usable */
    put_le64(hdr + 48, TDISK_BLOCKS - 34u);  /* last usable */
    put_le64(hdr + 72, g->ent_lba);
    put_le32(hdr + 80, g->ent_count);
    put_le32(hdr + 84, g->ent_size);
    put_le32(hdr + 88, ref_crc32(arr, (size_t)array_bytes));
    put_le32(hdr + 16, 0);
    put_le32(hdr + 16, ref_crc32(hdr, g->hdr_size));
}

/* Re-stamps both CRCs after a test has edited an entry in place, so the
 * parser is handed a table that is damaged only in the way the test meant. */
static void restamp(tdisk *d, const tgeom *g)
{
    uint8_t *hdr = d->buf + d->bs;
    uint8_t *arr = d->buf + g->ent_lba * d->bs;
    uint64_t array_bytes = (uint64_t)g->ent_count * g->ent_size;

    put_le32(hdr + 88, ref_crc32(arr, (size_t)array_bytes));
    put_le32(hdr + 16, 0);
    put_le32(hdr + 16, ref_crc32(hdr, g->hdr_size));
}

static uint8_t *entry_of(tdisk *d, const tgeom *g, uint32_t slot)
{
    return d->buf + g->ent_lba * d->bs + (uint64_t)slot * g->ent_size;
}

/* The falcon stick's own layout, which is what every acceptance test here
 * is ultimately checking the parser can read back. */
static const uint8_t guid_linux_luks[GPT_GUID_LEN] = {
    /* ca7d7ccb-63ed-4c53-861c-1742536059cc, p4's standard type */
    0xcb, 0x7c, 0x7d, 0xca, 0xed, 0x63, 0x53, 0x4c,
    0x86, 0x1c, 0x17, 0x42, 0x53, 0x60, 0x59, 0xcc
};

static void build_stick(tdisk *d)
{
    tgeom g;
    const tpart parts[] = {
        { gpt_guid_esp,     "FALCON-ESP",    2048,  34815, 0 },
        { gpt_guid_msbasic, "FALCON-ISO",   34816,  67583, 1 },
        { falcon_guid_shelf, "FALCON-UKI",  67584, 100351, 2 },
        { guid_linux_luks,  "FALCON-ROOT", 100352, 131038, 3 }
    };

    geom_default(&g);
    build_gpt(d, &g, parts, 4);
}

#define TABLE_CAP 16u

typedef struct {
    gpt_part parts[TABLE_CAP];
    gpt_table t;
} ttable;

static void ttable_init(ttable *tt, uint32_t cap)
{
    memset(tt, 0, sizeof(*tt));
    tt->t.parts = tt->parts;
    tt->t.cap = cap;
}

/* ---- the frozen constants ---- */

/* part.h calls these ABI with every stick ever written. This test is the
 * tripwire: changing a constant to "fix" something breaks here, loudly, with
 * the reason attached, rather than silently making old sticks unbootable. The
 * expected bytes are transcribed independently from the textual GUIDs in
 * part.h rather than copied from part.c. */
static void test_partition_types_are_frozen(void **state)
{
    /* ebd0a0a2-b9e5-4433-87c0-68b6b72699c7, Microsoft basic data, mixed-endian
     * on disk. p2 wears it so Windows gives p2 a drive letter; see part.h. */
    static const uint8_t msbasic[GPT_GUID_LEN] = {
        0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
    };
    /* 9cb02e90-85f4-4af7-9af3-1f9d6a95feaf */
    static const uint8_t shelf[GPT_GUID_LEN] = {
        0x90, 0x2e, 0xb0, 0x9c, 0xf4, 0x85, 0xf7, 0x4a,
        0x9a, 0xf3, 0x1f, 0x9d, 0x6a, 0x95, 0xfe, 0xaf
    };
    /* c12a7328-f81f-11d2-ba4b-00a0c93ec93b, the standard ESP type. */
    static const uint8_t esp[GPT_GUID_LEN] = {
        0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
    };

    (void)state;
    assert_memory_equal(gpt_guid_msbasic, msbasic, GPT_GUID_LEN);
    assert_memory_equal(falcon_guid_shelf, shelf, GPT_GUID_LEN);
    assert_memory_equal(gpt_guid_esp, esp, GPT_GUID_LEN);

    /* p3's minted type must never collide with a standard one; a stick
     * stamped with a duplicate would be ambiguous. p2's type deliberately IS
     * a standard one, which is exactly why its name is half its identity. */
    assert_int_equal(gpt_guid_eq(gpt_guid_msbasic, falcon_guid_shelf), 0);
    assert_int_equal(gpt_guid_eq(gpt_guid_msbasic, gpt_guid_esp), 0);
    assert_int_equal(gpt_guid_eq(falcon_guid_shelf, guid_linux_luks), 0);

    /* p2's name is the discriminator, and mkstick's MKS_LABEL_P2 is checked
     * against this same string at tool startup (falcon-cli/src/mkstick/guid.c).
     * */
    assert_string_equal(part_role_name(PART_ROLE_ISO), "FALCON-ISO");
    assert_string_equal(part_role_name(PART_ROLE_ISO), FALCON_PART_NAME_ISO);
    /* p3 is its type GUID and nothing else, so it has no name to match. */
    assert_null(part_role_name(PART_ROLE_SHELF));
    assert_null(part_role_name((part_role)7));

    assert_int_equal(gpt_guid_eq(gpt_guid_msbasic, gpt_guid_msbasic), 1);
    assert_int_equal(gpt_guid_eq(NULL, gpt_guid_msbasic), 0);
    assert_int_equal(gpt_guid_eq(gpt_guid_msbasic, NULL), 0);
}

/* ---- the identification rule (DESIGN Section 3) ---- */

/* Builds a 36-unit name field the way the disk carries one: no terminator is
 * implied, so `pad` says what fills the rest. */
static void make_name(uint16_t out[GPT_NAME_LEN], const char *ascii,
                      uint16_t pad)
{
    unsigned i;
    int ended = 0;

    for (i = 0; i < GPT_NAME_LEN; i++) {
        if (!ended && ascii[i] == '\0')
            ended = 1;
        out[i] = ended ? pad : (uint16_t)(unsigned char)ascii[i];
    }
}

/* The name field is attacker-editable data off removable media, so the
 * comparison has to survive every shape of it: no terminator anywhere, a
 * terminator in the middle with rubbish after it, a prefix of our name, our
 * name with something appended, and the wrong case. */
static void test_name_match_is_exact(void **state)
{
    uint16_t name[GPT_NAME_LEN];
    unsigned i;

    (void)state;

    /* The real thing: our name, NUL padding to 36 units. */
    make_name(name, "FALCON-ISO", 0);
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 1);

    /* All 36 units used, not one of them a NUL. Nothing may read past the
     * field looking for a terminator that is not there. */
    for (i = 0; i < GPT_NAME_LEN; i++)
        name[i] = (uint16_t)('A' + (i % 26u));
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);

    /* Our name packed out to all 36 units with 'X' instead of NULs: the
     * prefix matches and the field is still not ours. */
    make_name(name, "FALCON-ISO", 'X');
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);

    /* Our name, a NUL, then rubbish in the padding. A comparison that stopped
     * at the first NUL would call this ours. */
    make_name(name, "FALCON-ISO", 0);
    name[GPT_NAME_LEN - 1u] = 'Z';
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);
    make_name(name, "FALCON-ISO", 0);
    name[11] = '!';
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);

    /* An embedded NUL inside the name itself truncates it to something else. */
    make_name(name, "FALCON-ISO", 0);
    name[6] = 0;
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);

    /* A prefix, and our name with a suffix. */
    make_name(name, "FALCON-IS", 0);
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);
    make_name(name, "FALCON-ISOS", 0);
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);

    /* Case-sensitive, deliberately: part.h says why. */
    make_name(name, "falcon-iso", 0);
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);

    /* An empty field is a partition nobody named. */
    make_name(name, "", 0);
    assert_int_equal(gpt_name_eq(name, "FALCON-ISO"), 0);
    assert_int_equal(gpt_name_eq(name, ""), 1);

    /* A name longer than the field can hold never matches, and the compare
     * still reads only the 36 units that exist. */
    make_name(name, "FALCON-ISO", 0);
    assert_int_equal(
        gpt_name_eq(name,
                    "FALCON-ISO-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), 0);

    assert_int_equal(gpt_name_eq(NULL, "FALCON-ISO"), 0);
    assert_int_equal(gpt_name_eq(name, NULL), 0);
}

/* p2 needs BOTH halves: the type is Microsoft basic data, which every Windows
 * data partition in the machine wears, and the name is ours. Either alone
 * would aim the ISO scan at a stranger's volume. */
static void test_role_match_needs_type_and_name(void **state)
{
    uint16_t ours[GPT_NAME_LEN];
    uint16_t theirs[GPT_NAME_LEN];
    gpt_part p;

    (void)state;
    make_name(ours, "FALCON-ISO", 0);
    make_name(theirs, "Basic data partition", 0);

    /* Both halves: ours. */
    assert_int_equal(part_match_role(gpt_guid_msbasic, ours, PART_ROLE_ISO), 1);
    /* Right type, someone else's name: a Windows data partition. */
    assert_int_equal(part_match_role(gpt_guid_msbasic, theirs, PART_ROLE_ISO),
                     0);
    /* Right name, wrong type: a coincidence, or a label somebody chose. */
    assert_int_equal(part_match_role(falcon_guid_shelf, ours, PART_ROLE_ISO),
                     0);
    assert_int_equal(part_match_role(guid_linux_luks, ours, PART_ROLE_ISO), 0);
    assert_int_equal(part_match_role(gpt_guid_esp, ours, PART_ROLE_ISO), 0);
    /* No name established at all (the firmware route that could not read
     * one): half the rule is missing, so p2 does not match. */
    assert_int_equal(part_match_role(gpt_guid_msbasic, NULL, PART_ROLE_ISO), 0);

    /* p3 is its type GUID alone, whatever it is called. */
    assert_int_equal(part_match_role(falcon_guid_shelf, theirs,
                                     PART_ROLE_SHELF), 1);
    assert_int_equal(part_match_role(falcon_guid_shelf, NULL,
                                     PART_ROLE_SHELF), 1);
    assert_int_equal(part_match_role(gpt_guid_msbasic, ours,
                                     PART_ROLE_SHELF), 0);

    /* A role outside the enum matches nothing. */
    assert_int_equal(part_match_role(gpt_guid_msbasic, ours, (part_role)9), 0);

    /* The same rule over a decoded entry. */
    memset(&p, 0, sizeof p);
    memcpy(p.type, gpt_guid_msbasic, GPT_GUID_LEN);
    memcpy(p.name, ours, sizeof p.name);
    assert_int_equal(gpt_part_is_role(&p, PART_ROLE_ISO), 1);
    assert_int_equal(gpt_part_is_role(&p, PART_ROLE_SHELF), 0);
    memcpy(p.name, theirs, sizeof p.name);
    assert_int_equal(gpt_part_is_role(&p, PART_ROLE_ISO), 0);
    assert_int_equal(gpt_part_is_role(NULL, PART_ROLE_ISO), 0);
}

/* An internal disk with a Windows data partition on it is the case the name
 * half exists for: same type GUID as our p2, and it must not be claimed. */
static void test_windows_data_partition_is_not_ours(void **state)
{
    tdisk d;
    ttable tt;
    tgeom g;
    const tpart parts[] = {
        { gpt_guid_esp,     "EFI system partition",  2048, 34815, 0 },
        { gpt_guid_msbasic, "Basic data partition", 34816, 67583, 1 },
        /* And the mirror image: our name on a partition of a type we never
         * stamp it on. */
        { guid_linux_luks,  "FALCON-ISO",           67584, 100351, 2 }
    };

    (void)state;
    tdisk_init(&d, 512);
    geom_default(&g);
    build_gpt(&d, &g, parts, 3);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.count, 3);
    assert_int_equal(gpt_part_is_role(&tt.t.parts[1], PART_ROLE_ISO), 0);
    assert_int_equal(gpt_part_is_role(&tt.t.parts[2], PART_ROLE_ISO), 0);
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_ISO), 0);
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_SHELF), 0);

    tdisk_free(&d);
}

/* The decoder's own job: 36 units, little-endian, copied verbatim with no
 * terminator assumed. The shapes below are the ones a hand-edited or hostile
 * table produces, and the parser must carry them through unchanged so the
 * matcher gets to make the decision. */
static void test_name_field_decodes_verbatim(void **state)
{
    tdisk d;
    ttable tt;
    tgeom g;
    uint8_t *e;
    unsigned i;

    (void)state;
    tdisk_init(&d, 512);
    geom_default(&g);
    build_stick(&d);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.parts[1].name[0], 'F');
    assert_int_equal(tt.t.parts[1].name[9], 'O');
    assert_int_equal(tt.t.parts[1].name[10], 0);
    assert_int_equal(tt.t.parts[1].name[GPT_NAME_LEN - 1u], 0);
    assert_int_equal(gpt_part_is_role(&tt.t.parts[1], PART_ROLE_ISO), 1);

    /* All 36 units occupied, no NUL anywhere in the field. */
    build_stick(&d);
    e = entry_of(&d, &g, 1);
    for (i = 0; i < GPT_NAME_LEN; i++)
        put_le16(e + TENT_NAME + i * 2u, (uint16_t)('A' + (i % 26u)));
    restamp(&d, &g);
    ttable_init(&tt, TABLE_CAP);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.parts[1].name[GPT_NAME_LEN - 1u],
                     (uint16_t)('A' + ((GPT_NAME_LEN - 1u) % 26u)));
    assert_int_equal(gpt_part_is_role(&tt.t.parts[1], PART_ROLE_ISO), 0);

    /* Our name, a NUL, then more text: the padding is not padding. */
    build_stick(&d);
    e = entry_of(&d, &g, 1);
    put_le16(e + TENT_NAME + 11u * 2u, 'X');
    restamp(&d, &g);
    ttable_init(&tt, TABLE_CAP);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.parts[1].name[11], 'X');
    assert_int_equal(gpt_part_is_role(&tt.t.parts[1], PART_ROLE_ISO), 0);
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_ISO), 0);
    /* p3 is unaffected: its identity never involved a name. */
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_SHELF), 1);

    /* A unit above the ASCII range, stored little-endian, survives the
     * decode rather than being folded into a byte. */
    build_stick(&d);
    e = entry_of(&d, &g, 1);
    put_le16(e + TENT_NAME + 2u, 0x00E9u);
    restamp(&d, &g);
    ttable_init(&tt, TABLE_CAP);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.parts[1].name[1], 0x00E9u);
    assert_int_equal(gpt_part_is_role(&tt.t.parts[1], PART_ROLE_ISO), 0);

    tdisk_free(&d);
}

/* The reference CRC used to stamp every synthetic table above is the one the
 * GPT specification names, checked against its published value for the
 * string "123456789". Everything else here rests on this. */
static void test_reference_crc32_is_ieee(void **state)
{
    (void)state;
    assert_int_equal(ref_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    assert_int_equal(ref_crc32((const uint8_t *)"", 0), 0u);
}

/* ---- acceptance ---- */

static void test_parse_falcon_stick(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512);
    build_stick(&d);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.count, 4);
    assert_int_equal(tt.t.truncated, 0);

    assert_int_equal(tt.t.parts[0].number, 1);
    assert_memory_equal(tt.t.parts[0].type, gpt_guid_esp, GPT_GUID_LEN);
    assert_int_equal(tt.t.parts[0].start_lba, 2048);
    assert_int_equal(tt.t.parts[0].end_lba, 34815);

    assert_int_equal(tt.t.parts[1].number, 2);
    assert_memory_equal(tt.t.parts[1].type, gpt_guid_msbasic, GPT_GUID_LEN);
    assert_int_equal(tt.t.parts[1].start_lba, 34816);
    /* p2 is the one entry whose type does not identify it. */
    assert_int_equal(gpt_name_eq(tt.t.parts[1].name, "FALCON-ISO"), 1);
    assert_int_equal(gpt_part_is_role(&tt.t.parts[1], PART_ROLE_ISO), 1);
    assert_int_equal(gpt_part_is_role(&tt.t.parts[0], PART_ROLE_ISO), 0);

    assert_int_equal(tt.t.parts[2].number, 3);
    assert_memory_equal(tt.t.parts[2].type, falcon_guid_shelf, GPT_GUID_LEN);
    assert_int_equal(tt.t.parts[2].start_lba, 67584);

    assert_int_equal(tt.t.parts[3].number, 4);
    assert_memory_equal(tt.t.parts[3].type, guid_linux_luks, GPT_GUID_LEN);
    assert_int_equal(tt.t.parts[3].end_lba, 131038);

    /* The PARTUUID the install manifest keeps as a tiebreaker survives the
     * round trip, entry by entry. */
    assert_int_equal(tt.t.parts[1].unique[0], 0xA0u + 16u);

    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_ISO), 1);
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_SHELF), 1);
    assert_int_equal(gpt_has_role(&tt.t, (part_role)9), 0);
    assert_int_equal(gpt_has_role(NULL, PART_ROLE_ISO), 0);

    tdisk_free(&d);
}

/* The starting LBA is the key the EFI side uses to tie a HARDDRIVE device
 * path node back to its entry, so it has to resolve exactly and reject a
 * near miss rather than snapping to the nearest partition. */
static void test_find_by_start_lba(void **state)
{
    tdisk d;
    ttable tt;
    const gpt_part *p;

    (void)state;
    tdisk_init(&d, 512);
    build_stick(&d);
    ttable_init(&tt, TABLE_CAP);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);

    p = gpt_find_start(&tt.t, 34816);
    assert_non_null(p);
    assert_int_equal(p->number, 2);
    assert_memory_equal(p->type, gpt_guid_msbasic, GPT_GUID_LEN);

    assert_null(gpt_find_start(&tt.t, 34817));
    assert_null(gpt_find_start(&tt.t, 0));
    assert_null(gpt_find_start(NULL, 34816));

    tdisk_free(&d);
}

/* A disk carrying only standard types is the internal drive the loader must
 * not claim: nothing on it is ours, and the role queries must say so. */
static void test_foreign_disk_has_no_roles(void **state)
{
    tdisk d;
    ttable tt;
    tgeom g;
    const tpart parts[] = {
        { gpt_guid_esp,     NULL,  2048, 34815, 0 },
        { guid_linux_luks, NULL, 34816, 67583, 1 }
    };

    (void)state;
    tdisk_init(&d, 512);
    geom_default(&g);
    build_gpt(&d, &g, parts, 2);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.count, 2);
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_ISO), 0);
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_SHELF), 0);

    tdisk_free(&d);
}

/* Unused entries are holes, not partitions: the table must skip them and
 * still report the 1-based numbers the firmware and sgdisk use. */
static void test_sparse_entries_keep_numbers(void **state)
{
    tdisk d;
    ttable tt;
    tgeom g;
    const tpart parts[] = {
        { gpt_guid_esp,      NULL,  2048, 34815, 0 },
        { falcon_guid_shelf, NULL, 67584, 100351, 4 }
    };

    (void)state;
    tdisk_init(&d, 512);
    geom_default(&g);
    build_gpt(&d, &g, parts, 2);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.count, 2);
    assert_int_equal(tt.t.parts[0].number, 1);
    assert_int_equal(tt.t.parts[1].number, 5);

    tdisk_free(&d);
}

/* 4 KiB native sectors move the header and the array and change how many
 * entries land in one read, which is the whole of the block-size handling. */
static void test_parse_4k_blocks(void **state)
{
    tdisk d;
    ttable tt;
    tgeom g;
    const tpart parts[] = {
        { gpt_guid_msbasic,  "FALCON-ISO",  256, 4095, 0 },
        { falcon_guid_shelf, "FALCON-UKI", 4096, 8191, 1 }
    };

    (void)state;
    tdisk_init(&d, 4096);
    geom_default(&g);
    build_gpt(&d, &g, parts, 2);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 4096), GPT_OK);
    assert_int_equal(tt.t.count, 2);
    assert_memory_equal(tt.t.parts[0].type, gpt_guid_msbasic, GPT_GUID_LEN);
    assert_memory_equal(tt.t.parts[1].type, falcon_guid_shelf, GPT_GUID_LEN);

    tdisk_free(&d);
}

/* The caller owns the storage, so running out of it must be reported rather
 * than overrun; the entries that did fit stay valid. */
static void test_table_truncation_is_flagged(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512);
    build_stick(&d);
    ttable_init(&tt, 2);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_OK);
    assert_int_equal(tt.t.count, 2);
    assert_int_equal(tt.t.truncated, 1);
    assert_memory_equal(tt.t.parts[1].type, gpt_guid_msbasic, GPT_GUID_LEN);

    tdisk_free(&d);
}

/* ---- refusals ---- */

static void test_not_a_gpt(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512);
    ttable_init(&tt, TABLE_CAP);

    /* An all-zero disk: no signature at all. */
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_ERR_NOT_GPT);

    /* A signature that is nearly right is still not right. */
    build_stick(&d);
    d.buf[512 + 3] = 'p';
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_ERR_NOT_GPT);

    tdisk_free(&d);
}

/* A backup header copied over LBA 1 carries its own valid CRC but describes
 * itself as living at the end of the disk. Taking it would mean parsing an
 * array the header does not actually point at here. */
static void test_backup_header_at_lba1_refused(void **state)
{
    tdisk d;
    ttable tt;
    tgeom g;
    const tpart parts[] = { { gpt_guid_msbasic, "FALCON-ISO", 2048, 34815, 0 } };

    (void)state;
    tdisk_init(&d, 512);
    geom_default(&g);
    g.my_lba = TDISK_BLOCKS - 1u;
    build_gpt(&d, &g, parts, 1);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_ERR_NOT_GPT);
    tdisk_free(&d);
}

static void test_header_crc_refused(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512);
    build_stick(&d);
    ttable_init(&tt, TABLE_CAP);

    /* One bit inside the CRC'd region of the header. */
    d.buf[512 + 40] ^= 0x01u;
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_ERR_CRC);
    assert_int_equal(tt.t.count, 0);

    /* And the stored CRC itself. */
    build_stick(&d);
    d.buf[512 + 16] ^= 0xFFu;
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_ERR_CRC);

    tdisk_free(&d);
}

static void test_entry_array_crc_refused(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512);
    build_stick(&d);
    ttable_init(&tt, TABLE_CAP);

    /* Flip a type GUID byte. Without the array CRC this would parse happily
     * and report a partition type that is not on the disk, which is exactly
     * the failure that must never reach the menu. */
    d.buf[2 * 512 + 128 + 0] ^= 0x01u;
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_ERR_CRC);
    assert_int_equal(tt.t.count, 0);

    /* Damage in the array's trailing unused entries counts too: the CRC
     * covers the whole declared array, not just the used part. */
    build_stick(&d);
    d.buf[2 * 512 + 127 * 128 + 5] ^= 0x80u;
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512), GPT_ERR_CRC);

    tdisk_free(&d);
}

static void test_unsupported_geometry(void **state)
{
    tdisk d;
    ttable tt;
    tgeom g;
    const tpart parts[] = { { gpt_guid_msbasic, "FALCON-ISO", 2048, 34815, 0 } };

    (void)state;
    tdisk_init(&d, 512);
    ttable_init(&tt, TABLE_CAP);

    /* Header smaller than the fields the spec defines. */
    geom_default(&g);
    g.hdr_size = 91;
    build_gpt(&d, &g, parts, 1);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512),
                     GPT_ERR_UNSUPPORTED);

    /* Header larger than the block it must fit in. */
    geom_default(&g);
    g.hdr_size = 513;
    build_gpt(&d, &g, parts, 1);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512),
                     GPT_ERR_UNSUPPORTED);

    /* Entry smaller than the spec's 128 bytes. */
    geom_default(&g);
    g.ent_size = 64;
    build_gpt(&d, &g, parts, 1);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512),
                     GPT_ERR_UNSUPPORTED);

    /* Entry size that does not divide the block size: entries would straddle
     * block boundaries, which this parser declines rather than mis-reads. */
    geom_default(&g);
    g.ent_size = 192;
    g.ent_count = 64;
    build_gpt(&d, &g, parts, 1);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512),
                     GPT_ERR_UNSUPPORTED);

    /* An array that would overlap the header. */
    geom_default(&g);
    g.ent_lba = 1;
    build_gpt(&d, &g, parts, 1);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512),
                     GPT_ERR_UNSUPPORTED);

    /* An array bigger than GPT_ARRAY_MAX. Built by hand: the builder would
     * refuse to write one that does not fit the disk. */
    geom_default(&g);
    build_gpt(&d, &g, parts, 1);
    put_le32(d.buf + 512 + 80, 4096);   /* 4096 * 128 = 512 KiB */
    put_le32(d.buf + 512 + 16, 0);
    put_le32(d.buf + 512 + 16, ref_crc32(d.buf + 512, 92));
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512),
                     GPT_ERR_UNSUPPORTED);

    /* Zero entries at all. */
    geom_default(&g);
    build_gpt(&d, &g, parts, 1);
    put_le32(d.buf + 512 + 80, 0);
    put_le32(d.buf + 512 + 16, 0);
    put_le32(d.buf + 512 + 16, ref_crc32(d.buf + 512, 92));
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 512),
                     GPT_ERR_UNSUPPORTED);

    tdisk_free(&d);
}

static void test_bad_arguments(void **state)
{
    tdisk d;
    ttable tt;
    gpt_table empty;

    (void)state;
    tdisk_init(&d, 512);
    build_stick(&d);
    ttable_init(&tt, TABLE_CAP);

    assert_int_equal(gpt_parse(NULL, tdisk_read, &d, 512), GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_parse(&tt.t, NULL, &d, 512), GPT_ERR_BAD_ARG);

    memset(&empty, 0, sizeof(empty));
    assert_int_equal(gpt_parse(&empty, tdisk_read, &d, 512), GPT_ERR_BAD_ARG);
    empty.parts = tt.parts;
    assert_int_equal(gpt_parse(&empty, tdisk_read, &d, 512), GPT_ERR_BAD_ARG);

    /* Block sizes outside 512..GPT_MAX_SECTOR, and one that is in range but
     * not a power of two. */
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 0), GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 256), GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 8192), GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_parse(&tt.t, tdisk_read, &d, 1536), GPT_ERR_BAD_ARG);

    tdisk_free(&d);
}

/* Every read the parser makes has to be able to fail: a stick pulled mid-scan
 * is an ordinary event, and none of these may be reported as a valid table.
 * Call 0 is the header, call 1 the first block of the checksum pass, and the
 * decode pass re-reads the array after it. */
static void test_io_failure_sweep(void **state)
{
    unsigned call;

    (void)state;
    for (call = 0; call < 70u; call++) {
        tdisk d;
        ttable tt;
        int rc;

        tdisk_init(&d, 512);
        build_stick(&d);
        ttable_init(&tt, TABLE_CAP);
        d.fail_on = (int)call;

        rc = gpt_parse(&tt.t, tdisk_read, &d, 512);
        /* A standard table is 34 reads (1 header + 32 array blocks twice is
         * 65), so late call numbers stop failing anything; those must
         * succeed rather than report a phantom error. */
        if (d.calls > (int)call)
            assert_int_equal(rc, GPT_ERR_IO);
        else
            assert_int_equal(rc, GPT_OK);
        tdisk_free(&d);
    }
}

/* ---- the sgdisk-written table ---- */

/* Everything above is a table this file wrote. This one is a table gdisk wrote,
 * on a plain sparse file, stamped exactly as falcon-efi/tools/mk-test-stick.sh
 * and mkstick stamp a real one - p2's Microsoft basic data type AND its name
 * included, since sgdisk's -c is now what identifies p2. It is the only check
 * here that the parser and the tool that makes our sticks agree about the
 * bytes, and the only one that proves sgdisk writes the name field where the
 * spec says it is.
 */
typedef struct {
    int fd;
} fdisk;

static int fdisk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    fdisk *f = ctx;
    ssize_t got = pread(f->fd, buf, (size_t)len, (off_t)off);

    return (got == (ssize_t)len) ? 0 : -1;
}

static int have_sgdisk(void)
{
    return system("command -v sgdisk >/dev/null 2>&1") == 0;
}

static void test_sgdisk_written_table(void **state)
{
    char path[64];
    char cmd[768];
    fdisk f;
    ttable tt;
    const gpt_part *p;
    int fd;
    int rc;

    (void)state;
    if (!have_sgdisk()) {
        printf("\n[  SKIPPED ] sgdisk not found; "
               "synthetic tables still cover the parser.\n");
        return;
    }

    snprintf(path, sizeof(path), "/tmp/falcon-part-%ld.img", (long)getpid());
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert_true(fd >= 0);
    /* Sparse: nothing but the two GPTs is ever written. */
    assert_int_equal(ftruncate(fd, 128 * 1024 * 1024), 0);

    snprintf(cmd, sizeof(cmd),
             "sgdisk -n 1:2048:+16M -t 1:C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
             " -c 1:FALCON-ESP"
             " -n 2:0:+16M -t 2:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
             " -c 2:FALCON-ISO"
             " -n 3:0:+16M -t 3:9CB02E90-85F4-4AF7-9AF3-1F9D6A95FEAF"
             " -c 3:FALCON-UKI"
             " -n 4:0:0 -t 4:CA7D7CCB-63ED-4C53-861C-1742536059CC"
             " -c 4:FALCON-ROOT"
             " %s >/dev/null 2>&1", path);
    rc = system(cmd);
    assert_int_equal(rc, 0);

    f.fd = fd;
    ttable_init(&tt, TABLE_CAP);
    assert_int_equal(gpt_parse(&tt.t, fdisk_read, &f, 512), GPT_OK);
    assert_int_equal(tt.t.count, 4);
    assert_int_equal(tt.t.truncated, 0);

    assert_memory_equal(tt.t.parts[0].type, gpt_guid_esp, GPT_GUID_LEN);
    assert_memory_equal(tt.t.parts[1].type, gpt_guid_msbasic, GPT_GUID_LEN);
    assert_memory_equal(tt.t.parts[2].type, falcon_guid_shelf, GPT_GUID_LEN);
    assert_memory_equal(tt.t.parts[3].type, guid_linux_luks, GPT_GUID_LEN);

    assert_int_equal(tt.t.parts[0].start_lba, 2048);
    assert_int_equal(tt.t.parts[1].start_lba, 34816);
    assert_int_equal(tt.t.parts[2].start_lba, 67584);

    /* The whole point of the change of 2026-09-13: sgdisk's -c lands in the
     * name field the parser reads, so the identification rule holds over a
     * table we did not write ourselves. */
    assert_int_equal(gpt_name_eq(tt.t.parts[1].name, "FALCON-ISO"), 1);
    assert_int_equal(gpt_part_is_role(&tt.t.parts[1], PART_ROLE_ISO), 1);
    assert_int_equal(gpt_name_eq(tt.t.parts[2].name, "FALCON-UKI"), 1);

    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_ISO), 1);
    assert_int_equal(gpt_has_role(&tt.t, PART_ROLE_SHELF), 1);

    p = gpt_find_start(&tt.t, 34816);
    assert_non_null(p);
    assert_int_equal(p->number, 2);

    close(fd);
    unlink(path);
}

/* A file with no partition table at all: the fallback route walks every whole
 * disk in the machine, so refusing one cleanly is a normal event, not an
 * error path. */
static void test_unpartitioned_file_not_gpt(void **state)
{
    char path[64];
    fdisk f;
    ttable tt;
    int fd;

    (void)state;
    snprintf(path, sizeof(path), "/tmp/falcon-raw-%ld.img", (long)getpid());
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert_true(fd >= 0);
    assert_int_equal(ftruncate(fd, 1024 * 1024), 0);

    f.fd = fd;
    ttable_init(&tt, TABLE_CAP);
    assert_int_equal(gpt_parse(&tt.t, fdisk_read, &f, 512), GPT_ERR_NOT_GPT);

    close(fd);
    unlink(path);
}

int run_part_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_partition_types_are_frozen),
        cmocka_unit_test(test_reference_crc32_is_ieee),
        cmocka_unit_test(test_name_match_is_exact),
        cmocka_unit_test(test_role_match_needs_type_and_name),
        cmocka_unit_test(test_windows_data_partition_is_not_ours),
        cmocka_unit_test(test_name_field_decodes_verbatim),
        cmocka_unit_test(test_parse_falcon_stick),
        cmocka_unit_test(test_find_by_start_lba),
        cmocka_unit_test(test_foreign_disk_has_no_roles),
        cmocka_unit_test(test_sparse_entries_keep_numbers),
        cmocka_unit_test(test_parse_4k_blocks),
        cmocka_unit_test(test_table_truncation_is_flagged),
        cmocka_unit_test(test_not_a_gpt),
        cmocka_unit_test(test_backup_header_at_lba1_refused),
        cmocka_unit_test(test_header_crc_refused),
        cmocka_unit_test(test_entry_array_crc_refused),
        cmocka_unit_test(test_unsupported_geometry),
        cmocka_unit_test(test_bad_arguments),
        cmocka_unit_test(test_io_failure_sweep),
        cmocka_unit_test(test_sgdisk_written_table),
        cmocka_unit_test(test_unpartitioned_file_not_gpt),
    };

    return cmocka_run_group_tests_name("part", tests, NULL, NULL);
}
