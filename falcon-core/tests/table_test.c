/* Tests for falcon-core/src/part/table.c: a disk's MBR or GPT as one list
 * (docs/DESIGN.md Section 3.2).
 *
 * Every disk here is built byte by byte in memory, for the reason
 * part_test.c gives: the rejection paths (a looping extended chain, a
 * primary GPT with a bad CRC and a good backup, a table whose entries
 * overlap) are ones no partitioning tool will write on purpose. Offsets are
 * spelt here rather than borrowed from table.c, so the test disagrees with
 * the parser when the parser is wrong, and the GPT CRCs are stamped with a
 * bit-at-a-time reference rather than part.c's table.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include <part/table.h>

/* ---- a disk in memory ---- */

typedef struct {
    uint8_t *buf;
    uint64_t blocks;
    uint32_t bs;
    int      fail_on;    /* -1 never; else the 0-based call that fails */
    int      calls;
    int64_t  fail_lba;   /* -1 never; else any read starting at this block */
} tdisk;

static int tdisk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    tdisk *d = ctx;
    int call = d->calls++;

    if (d->fail_on >= 0 && call >= d->fail_on)
        return -1;
    if (d->fail_lba >= 0 && off == (uint64_t)d->fail_lba * d->bs)
        return -1;
    /* Whole blocks, as the loaders' callbacks take them. */
    assert_int_equal(off % d->bs, 0);
    assert_int_equal(len % d->bs, 0);
    if (off + len > d->blocks * d->bs)
        return -1;
    memcpy(buf, d->buf + off, (size_t)len);
    return 0;
}

static void tdisk_init(tdisk *d, uint32_t bs, uint64_t blocks)
{
    d->bs = bs;
    d->blocks = blocks;
    d->buf = calloc(1, (size_t)(blocks * bs));
    assert_non_null(d->buf);
    d->fail_on = -1;
    d->calls = 0;
    d->fail_lba = -1;
}

static void tdisk_free(tdisk *d)
{
    free(d->buf);
    d->buf = NULL;
}

static uint8_t *blk(tdisk *d, uint64_t lba)
{
    return d->buf + lba * d->bs;
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

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

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

/* ---- MBR builder ---- */

/* One 16-byte entry at slot 0-3 of the MBR or EBR at `lba`, and the
 * 0x55AA behind them. CHS left zero: nothing reads it. */
static void mbr_entry(tdisk *d, uint64_t lba, unsigned slot, uint8_t status,
                      uint8_t type, uint32_t start, uint32_t count)
{
    uint8_t *s = blk(d, lba);
    uint8_t *e = s + 446 + slot * 16u;

    e[0] = status;
    e[4] = type;
    put_le32(e + 8, start);
    put_le32(e + 12, count);
    s[510] = 0x55;
    s[511] = 0xAA;
}

static void mbr_sig(tdisk *d, uint32_t sig)
{
    put_le32(blk(d, 0) + 440, sig);
}

/* ---- GPT builder ---- */

typedef struct {
    uint8_t  type0;       /* first byte of a type GUID; 0 = unused */
    uint64_t start;
    uint64_t end;
    uint32_t slot;
} tgpt;

#define TGPT_ENTS 128u
#define TGPT_ESZ  128u

static const uint8_t disk_guid[16] = {
    0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};

/* One copy of the table: header at hdr_lba, array at ent_lba, CRCs
 * stamped. The array is the full 128 x 128 = 16 KiB, so it takes
 * 32 blocks at 512 and 4 at 4096. */
static void gpt_copy(tdisk *d, uint64_t hdr_lba, uint64_t alt_lba,
                     uint64_t ent_lba, const tgpt *parts, unsigned n)
{
    uint8_t *hdr = blk(d, hdr_lba);
    uint8_t *arr = blk(d, ent_lba);
    unsigned i, k;

    memset(hdr, 0, d->bs);
    memset(arr, 0, TGPT_ENTS * TGPT_ESZ);
    for (i = 0; i < n; i++) {
        uint8_t *e = arr + parts[i].slot * TGPT_ESZ;

        if (parts[i].type0 == 0)
            continue;
        e[0] = parts[i].type0;
        e[15] = 0x5a;
        for (k = 0; k < 16; k++)
            e[16 + k] = (uint8_t)(0x10u * parts[i].slot + k);
        put_le64(e + 32, parts[i].start);
        put_le64(e + 40, parts[i].end);
        put_le16(e + 56, 'p');
        put_le16(e + 58, (uint16_t)('0' + parts[i].slot));
    }
    memcpy(hdr, "EFI PART", 8);
    put_le32(hdr + 8, 0x00010000u);
    put_le32(hdr + 12, 92);
    put_le64(hdr + 24, hdr_lba);
    put_le64(hdr + 32, alt_lba);
    put_le64(hdr + 40, 34);
    put_le64(hdr + 48, d->blocks - 34u);
    memcpy(hdr + 56, disk_guid, 16);
    put_le64(hdr + 72, ent_lba);
    put_le32(hdr + 80, TGPT_ENTS);
    put_le32(hdr + 84, TGPT_ESZ);
    put_le32(hdr + 88, ref_crc32(arr, TGPT_ENTS * TGPT_ESZ));
    put_le32(hdr + 16, ref_crc32(hdr, 92));
}

static uint64_t array_blocks(const tdisk *d)
{
    return (TGPT_ENTS * TGPT_ESZ) / d->bs;
}

/* A protective MBR and both copies, the way sgdisk writes a disk. */
static void gpt_disk(tdisk *d, const tgpt *parts, unsigned n)
{
    uint64_t last = d->blocks - 1u;

    mbr_entry(d, 0, 0, 0x00, 0xEE, 1,
              (uint32_t)(d->blocks - 1u > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                                      : d->blocks - 1u));
    gpt_copy(d, 1, last, 2, parts, n);
    gpt_copy(d, last, 1, last - array_blocks(d), parts, n);
}

/* ---- the table under test ---- */

#define CAP 80u

typedef struct {
    part_entry parts[CAP];
    part_table t;
} ttable;

static void tt_init(ttable *tt, uint32_t cap)
{
    memset(tt, 0, sizeof(*tt));
    tt->t.parts = tt->parts;
    tt->t.cap = cap;
}

static int rd(ttable *tt, tdisk *d, uint64_t blocks)
{
    return part_read_table(&tt->t, tdisk_read, d, d->bs, blocks);
}

/* ---- MBR ---- */

static void test_mbr_plain(void **state)
{
    tdisk d;
    ttable tt;
    const part_entry *p;

    (void)state;
    tdisk_init(&d, 512, 1000);
    mbr_sig(&d, 0xA1B2C3D4u);
    mbr_entry(&d, 0, 0, 0x80, 0x07, 63, 100);
    mbr_entry(&d, 0, 2, 0x00, 0x83, 200, 50);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 1000), PART_OK);

    assert_int_equal(tt.t.count, 2);
    assert_int_equal(tt.t.id.kind, PART_KIND_MBR);
    assert_int_equal(tt.t.id.mbr_sig, 0xA1B2C3D4u);
    assert_int_equal(tt.t.first_usable, 1);
    assert_int_equal(tt.t.last_usable, 999);
    assert_int_equal(tt.t.first_start, 63);
    assert_int_equal(tt.t.used_backup, 0);
    assert_int_equal(tt.t.truncated, 0);

    p = &tt.t.parts[0];
    assert_int_equal(p->number, 1);
    assert_int_equal(p->start_lba, 63);
    assert_int_equal(p->end_lba, 162);
    assert_int_equal(p->active, 1);
    assert_int_equal(p->primary, 1);
    assert_int_equal(p->mbr_type, 0x07);
    /* Slot 2 is empty, and the next primary is still 3. */
    p = &tt.t.parts[1];
    assert_int_equal(p->number, 3);
    assert_int_equal(p->active, 0);
    assert_int_equal(p->mbr_type, 0x83);

    assert_ptr_equal(part_find_start(&tt.t, 200), &tt.t.parts[1]);
    assert_null(part_find_start(&tt.t, 201));
    assert_null(part_find_start(NULL, 200));

    /* The length unknown: no end checks, and no last block to report. */
    assert_int_equal(rd(&tt, &d, 0), PART_OK);
    assert_int_equal(tt.t.last_usable, 0);
    tdisk_free(&d);
}

/* An extended partition at 500..899 holding three logicals, the second
 * EBR's link given relative to the extended partition as the format says. */
static void build_chain(tdisk *d)
{
    mbr_entry(d, 0, 0, 0x80, 0x0C, 2048 - 1000, 400);   /* 1048..1447 */
    mbr_entry(d, 0, 1, 0x00, 0x0F, 500, 400);           /* 500..899 */
    mbr_entry(d, 500, 0, 0x00, 0x83, 1, 50);            /* 501..550 */
    mbr_entry(d, 500, 1, 0x00, 0x05, 100, 60);          /* EBR at 600 */
    mbr_entry(d, 600, 0, 0x80, 0x82, 2, 50);            /* 602..651 */
    mbr_entry(d, 600, 1, 0x00, 0x05, 200, 20);          /* EBR at 700 */
    mbr_entry(d, 700, 0, 0x00, 0x07, 1, 10);            /* 701..710 */
}

static void test_mbr_extended_chain(void **state)
{
    tdisk d;
    ttable tt;
    const part_entry *p;

    (void)state;
    tdisk_init(&d, 512, 2000);
    build_chain(&d);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);

    /* Two primaries, the container among them, then three logicals. */
    assert_int_equal(tt.t.count, 5);
    p = &tt.t.parts[1];
    assert_int_equal(p->number, 2);
    assert_int_equal(p->mbr_type, 0x0F);
    assert_int_equal(part_mbr_is_extended(p->mbr_type), 1);

    p = &tt.t.parts[2];
    assert_int_equal(p->number, 5);
    assert_int_equal(p->primary, 0);
    assert_int_equal(p->start_lba, 501);
    assert_int_equal(p->end_lba, 550);
    p = &tt.t.parts[3];
    assert_int_equal(p->number, 6);
    assert_int_equal(p->start_lba, 602);
    assert_int_equal(p->active, 1);
    assert_int_equal(p->mbr_type, 0x82);
    p = &tt.t.parts[4];
    assert_int_equal(p->number, 7);
    assert_int_equal(p->start_lba, 701);
    assert_int_equal(p->end_lba, 710);

    assert_int_equal(tt.t.first_start, 500);
    tdisk_free(&d);
}

/* An empty first EBR (a link and no data entry) is what an extended
 * partition whose first logical was deleted looks like; the walk goes on. */
static void test_mbr_chain_empty_first_ebr(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 2000);
    mbr_entry(&d, 0, 3, 0x00, 0x85, 500, 400);
    mbr_entry(&d, 500, 1, 0x00, 0x05, 100, 60);
    mbr_entry(&d, 600, 0, 0x00, 0x83, 2, 50);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 2);
    assert_int_equal(tt.t.parts[0].number, 4);
    assert_int_equal(tt.t.parts[1].number, 5);
    assert_int_equal(tt.t.parts[1].start_lba, 602);

    /* The first data entry and the first link are the ones taken: a
     * second of either, and a slot with sectors but no type, are passed
     * over. */
    mbr_entry(&d, 600, 1, 0x00, 0x00, 300, 5);
    mbr_entry(&d, 600, 2, 0x00, 0x83, 60, 5);
    mbr_entry(&d, 600, 3, 0x00, 0x05, 200, 20);
    mbr_entry(&d, 700, 0, 0x00, 0x83, 1, 5);
    mbr_entry(&d, 700, 1, 0x00, 0x05, 250, 20);
    mbr_entry(&d, 700, 2, 0x00, 0x05, 150, 20);
    mbr_entry(&d, 750, 0, 0x00, 0x83, 1, 5);
    mbr_entry(&d, 650, 0, 0x00, 0x83, 1, 5);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 4);
    assert_int_equal(tt.t.parts[2].start_lba, 701);
    assert_int_equal(tt.t.parts[3].start_lba, 751);
    tdisk_free(&d);
}

static void test_mbr_looping_chain(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 2000);
    build_chain(&d);
    /* The third EBR links back to the first. */
    mbr_entry(&d, 700, 1, 0x00, 0x05, 0, 60);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 5);
    assert_int_equal(tt.t.truncated, 0);

    /* And one that links to itself. */
    mbr_entry(&d, 600, 1, 0x00, 0x05, 100, 60);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 4);
    tdisk_free(&d);
}

static void test_mbr_escaping_chain(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 2000);
    build_chain(&d);
    /* The second link points past the extended partition's end (899). */
    mbr_entry(&d, 600, 1, 0x00, 0x05, 450, 20);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 4);
    assert_int_equal(tt.t.parts[3].number, 6);

    /* A logical running past the container ends the walk before it. */
    build_chain(&d);
    mbr_entry(&d, 600, 0, 0x00, 0x83, 2, 400);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 3);

    /* So does one starting on its own EBR. */
    build_chain(&d);
    mbr_entry(&d, 600, 0, 0x00, 0x83, 0, 5);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 3);

    /* And an EBR without the signature. */
    build_chain(&d);
    blk(&d, 700)[511] = 0;
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 4);
    tdisk_free(&d);
}

/* More EBRs than PART_MAX_LOGICAL, none of them a loop: the bound ends the
 * walk and says the table is incomplete. */
static void test_mbr_chain_bound(void **state)
{
    tdisk d;
    ttable tt;
    uint32_t i;

    (void)state;
    tdisk_init(&d, 512, 1000);
    mbr_entry(&d, 0, 0, 0x00, 0x05, 100, 800);
    for (i = 0; i < PART_MAX_LOGICAL + 5u; i++) {
        uint64_t ebr = 100u + i * 4u;

        mbr_entry(&d, ebr, 0, 0x00, 0x83, 1, 2);
        mbr_entry(&d, ebr, 1, 0x00, 0x05, (i + 1u) * 4u, 4);
    }
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 1000), PART_OK);
    assert_int_equal(tt.t.count, 1u + PART_MAX_LOGICAL);
    assert_int_equal(tt.t.truncated, 1);
    assert_int_equal(tt.t.parts[PART_MAX_LOGICAL].number,
                     4u + PART_MAX_LOGICAL);
    tdisk_free(&d);
}

static void test_mbr_truncation(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 2000);
    build_chain(&d);
    tt_init(&tt, 3);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 3);
    assert_int_equal(tt.t.truncated, 1);

    tt_init(&tt, 1);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 1);
    assert_int_equal(tt.t.truncated, 1);
    tdisk_free(&d);
}

static void test_mbr_corrupt(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tt_init(&tt, CAP);

    /* Two primaries sharing blocks. */
    tdisk_init(&d, 512, 1000);
    mbr_entry(&d, 0, 0, 0x00, 0x07, 100, 100);
    mbr_entry(&d, 0, 1, 0x00, 0x83, 199, 10);
    assert_int_equal(rd(&tt, &d, 1000), PART_ERR_CORRUPT);
    assert_int_equal(tt.t.count, 0);
    tdisk_free(&d);

    /* A primary past the end of the disk; fine when the length is
     * unknown. */
    tdisk_init(&d, 512, 1000);
    mbr_entry(&d, 0, 0, 0x00, 0x07, 900, 101);
    assert_int_equal(rd(&tt, &d, 1000), PART_ERR_CORRUPT);
    assert_int_equal(rd(&tt, &d, 0), PART_OK);
    tdisk_free(&d);

    /* A primary on LBA 0 itself. */
    tdisk_init(&d, 512, 1000);
    mbr_entry(&d, 0, 0, 0x00, 0x07, 0, 10);
    assert_int_equal(rd(&tt, &d, 1000), PART_ERR_CORRUPT);
    tdisk_free(&d);

    /* An extended partition past the end is refused before its chain is
     * walked. */
    tdisk_init(&d, 512, 1000);
    mbr_entry(&d, 0, 0, 0x00, 0x05, 500, 600);
    assert_int_equal(rd(&tt, &d, 1000), PART_ERR_CORRUPT);
    tdisk_free(&d);

    /* A logical overlapping a primary outside the container. */
    tdisk_init(&d, 512, 2000);
    build_chain(&d);
    mbr_entry(&d, 0, 2, 0x00, 0x07, 900, 100);          /* 900..999 */
    mbr_entry(&d, 0, 1, 0x00, 0x0F, 500, 500);          /* 500..999 */
    assert_int_equal(rd(&tt, &d, 2000), PART_ERR_CORRUPT);
    tdisk_free(&d);

    /* Two logicals sharing blocks. */
    tdisk_init(&d, 512, 2000);
    build_chain(&d);
    mbr_entry(&d, 600, 0, 0x00, 0x83, 2, 105);          /* 602..706 */
    assert_int_equal(rd(&tt, &d, 2000), PART_ERR_CORRUPT);
    tdisk_free(&d);
}

/* A 0x55AA sector that is not a table: nothing at all, a boot sector whose
 * table area is empty, and a FAT boot sector carrying an identity record,
 * whose "FLCN" puts 'C' in entry 3's status byte. */
static void test_no_table(void **state)
{
    tdisk d;
    ttable tt;
    uint8_t *s;

    (void)state;
    tt_init(&tt, CAP);
    tdisk_init(&d, 512, 2880);
    assert_int_equal(rd(&tt, &d, 2880), PART_ERR_NO_TABLE);

    s = blk(&d, 0);
    s[0] = 0xEB;
    s[1] = 0x3C;
    s[2] = 0x90;
    s[510] = 0x55;
    s[511] = 0xAA;
    assert_int_equal(rd(&tt, &d, 2880), PART_ERR_NO_TABLE);

    /* A type but no sectors is an empty slot, as Linux reads it. */
    mbr_entry(&d, 0, 0, 0x00, 0x0C, 5, 0);
    assert_int_equal(rd(&tt, &d, 2880), PART_ERR_NO_TABLE);

    memcpy(s + 0x1DC, "FLCN", 4);
    s[0x1E0] = 1;
    s[0x1E1] = 1;
    mbr_entry(&d, 0, 0, 0x00, 0x0C, 5, 100);
    assert_int_equal(rd(&tt, &d, 2880), PART_ERR_NO_TABLE);

    /* Any status other than 0x00 or 0x80 does the same. */
    memset(s + 0x1BE, 0, 64);
    mbr_entry(&d, 0, 1, 0x01, 0x0C, 5, 100);
    assert_int_equal(rd(&tt, &d, 2880), PART_ERR_NO_TABLE);
    tdisk_free(&d);
}

/* ---- GPT ---- */

static const tgpt stick[] = {
    { 0xC1, 2048,  3071, 0 },
    { 0xEB, 3072,  5119, 1 },
    { 0x9C, 5120,  6143, 3 },     /* slot 2 empty: numbers keep the gap */
};

static void check_stick(const ttable *tt)
{
    const part_entry *p;

    assert_int_equal(tt->t.count, 3);
    assert_int_equal(tt->t.id.kind, PART_KIND_GPT);
    assert_memory_equal(tt->t.id.guid, disk_guid, 16);
    assert_int_equal(tt->t.first_usable, 34);
    assert_int_equal(tt->t.first_start, 2048);

    p = &tt->t.parts[2];
    assert_int_equal(p->number, 4);
    assert_int_equal(p->start_lba, 5120);
    assert_int_equal(p->end_lba, 6143);
    assert_int_equal(p->primary, 1);
    assert_int_equal(p->active, 0);
    assert_int_equal(p->mbr_type, 0);
    assert_int_equal(p->type_guid[0], 0x9C);
    assert_int_equal(p->type_guid[15], 0x5a);
    assert_int_equal(p->unique[0], 0x30);
    assert_int_equal(p->name[0], 'p');
    assert_int_equal(p->name[1], '3');
    assert_int_equal(p->name[2], 0);
}

static void test_gpt_protective(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 8192);
    gpt_disk(&d, stick, 3);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    check_stick(&tt);
    assert_int_equal(tt.t.used_backup, 0);
    assert_int_equal(tt.t.last_usable, 8192 - 34);

    /* Length unknown: the primary is enough. */
    assert_int_equal(rd(&tt, &d, 0), PART_OK);
    check_stick(&tt);

    tt_init(&tt, 2);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    assert_int_equal(tt.t.count, 2);
    assert_int_equal(tt.t.truncated, 1);
    tdisk_free(&d);
}

/* A hybrid MBR: the 0xEE entry and real-looking ones beside it. The GPT
 * is the table, and the MBR's other entries are never listed. */
static void test_gpt_hybrid(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 8192);
    gpt_disk(&d, stick, 3);
    memset(blk(&d, 0) + 446, 0, 64);
    mbr_entry(&d, 0, 0, 0x80, 0x0C, 2048, 1024);
    mbr_entry(&d, 0, 1, 0x00, 0x07, 3072, 2048);
    mbr_entry(&d, 0, 3, 0x00, 0xEE, 1, 2047);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    check_stick(&tt);
    tdisk_free(&d);
}

static void test_gpt_backup(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 8192);
    gpt_disk(&d, stick, 3);
    blk(&d, 1)[20] ^= 0xFF;                 /* primary header CRC */
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    assert_int_equal(tt.t.used_backup, 1);
    check_stick(&tt);

    /* The primary's array rather than its header. */
    gpt_disk(&d, stick, 3);
    blk(&d, 2)[40] ^= 0xFF;
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    assert_int_equal(tt.t.used_backup, 1);
    check_stick(&tt);

    /* No length, no backup to find. */
    assert_int_equal(rd(&tt, &d, 0), PART_ERR_CORRUPT);
    assert_int_equal(tt.t.count, 0);

    /* Both copies bad. */
    blk(&d, 8191)[20] ^= 0xFF;
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_CORRUPT);
    assert_int_equal(tt.t.used_backup, 0);

    /* The primary gone entirely (a disk whose start was overwritten but
     * whose protective MBR survived): the backup still reads. */
    gpt_disk(&d, stick, 3);
    memset(blk(&d, 1), 0, 512);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    assert_int_equal(tt.t.used_backup, 1);
    tdisk_free(&d);
}

/* Geometry the parser declines, in both copies, is reported as that rather
 * than as damage; in one copy, the other is used. */
static void test_gpt_unsupported(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 8192);
    gpt_disk(&d, stick, 3);
    put_le32(blk(&d, 1) + 84, 64);
    put_le32(blk(&d, 1) + 16, 0);
    put_le32(blk(&d, 1) + 16, ref_crc32(blk(&d, 1), 92));
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    assert_int_equal(tt.t.used_backup, 1);
    assert_int_equal(rd(&tt, &d, 0), PART_ERR_UNSUPPORTED);

    put_le32(blk(&d, 8191) + 84, 64);
    put_le32(blk(&d, 8191) + 16, 0);
    put_le32(blk(&d, 8191) + 16, ref_crc32(blk(&d, 8191), 92));
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_UNSUPPORTED);

    /* Unsupported primary and a corrupt backup is damage. */
    blk(&d, 8191)[20] ^= 0xFF;
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_CORRUPT);
    tdisk_free(&d);
}

static void test_gpt_corrupt_entries(void **state)
{
    tdisk d;
    ttable tt;
    const tgpt overlap[] = {
        { 0xC1, 2048, 3071, 0 },
        { 0xEB, 3000, 5119, 1 },
    };
    const tgpt past_end[] = { { 0xC1, 2048, 8192, 0 } };
    const tgpt reversed[] = { { 0xC1, 3000, 2048, 0 } };
    const tgpt at_zero[] = { { 0xC1, 0, 10, 0 } };

    (void)state;
    tdisk_init(&d, 512, 8192);
    tt_init(&tt, CAP);
    gpt_disk(&d, overlap, 2);
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_CORRUPT);
    gpt_disk(&d, past_end, 1);
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_CORRUPT);
    assert_int_equal(rd(&tt, &d, 0), PART_OK);
    gpt_disk(&d, reversed, 1);
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_CORRUPT);
    gpt_disk(&d, at_zero, 1);
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_CORRUPT);

    /* A table with no partitions is a table: empty, not absent. */
    gpt_disk(&d, NULL, 0);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    assert_int_equal(tt.t.count, 0);
    assert_int_equal(tt.t.first_start, 0);
    tdisk_free(&d);
}

/* 4Kn: the same tables in 4096-byte blocks. The GPT's array is 4 blocks,
 * the MBR's fields are in the first 512 bytes of LBA 0 and of each EBR. */
static void test_4k_sectors(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 4096, 8192);
    gpt_disk(&d, stick, 3);
    tt_init(&tt, CAP);
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    check_stick(&tt);
    blk(&d, 1)[20] ^= 0xFF;
    assert_int_equal(rd(&tt, &d, 8192), PART_OK);
    assert_int_equal(tt.t.used_backup, 1);
    check_stick(&tt);
    tdisk_free(&d);

    tdisk_init(&d, 4096, 2000);
    build_chain(&d);
    assert_int_equal(rd(&tt, &d, 2000), PART_OK);
    assert_int_equal(tt.t.count, 5);
    assert_int_equal(tt.t.parts[4].start_lba, 701);
    tdisk_free(&d);
}

static void test_io_errors(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tt_init(&tt, CAP);

    tdisk_init(&d, 512, 8192);
    gpt_disk(&d, stick, 3);
    d.fail_on = 0;
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_IO);
    d.fail_on = -1;
    d.fail_lba = 1;                          /* primary header */
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_IO);
    d.fail_lba = 8191;                       /* backup, primary bad */
    blk(&d, 1)[20] ^= 0xFF;
    assert_int_equal(rd(&tt, &d, 8192), PART_ERR_IO);
    tdisk_free(&d);

    tdisk_init(&d, 512, 2000);
    build_chain(&d);
    d.fail_lba = 600;                        /* an EBR */
    assert_int_equal(rd(&tt, &d, 2000), PART_ERR_IO);
    assert_int_equal(tt.t.count, 0);
    tdisk_free(&d);
}

static void test_bad_args(void **state)
{
    tdisk d;
    ttable tt;

    (void)state;
    tdisk_init(&d, 512, 100);
    tt_init(&tt, CAP);
    assert_int_equal(part_read_table(NULL, tdisk_read, &d, 512, 0),
                     PART_ERR_ARG);
    assert_int_equal(part_read_table(&tt.t, NULL, &d, 512, 0), PART_ERR_ARG);
    assert_int_equal(part_read_table(&tt.t, tdisk_read, &d, 1024 + 512, 0),
                     PART_ERR_ARG);
    assert_int_equal(part_read_table(&tt.t, tdisk_read, &d, 256, 0),
                     PART_ERR_ARG);
    assert_int_equal(part_read_table(&tt.t, tdisk_read, &d, 8192, 0),
                     PART_ERR_ARG);
    assert_int_equal(part_read_table(&tt.t, tdisk_read, &d, 512,
                                     UINT64_MAX / 256u), PART_ERR_ARG);
    tt.t.cap = 0;
    assert_int_equal(part_read_table(&tt.t, tdisk_read, &d, 512, 0),
                     PART_ERR_ARG);
    tt.t.cap = CAP;
    tt.t.parts = NULL;
    assert_int_equal(part_read_table(&tt.t, tdisk_read, &d, 512, 0),
                     PART_ERR_ARG);
    tdisk_free(&d);
}

/* gpt_read_at's own edges, which gpt_parse never reaches. */
static void test_gpt_read_at_args(void **state)
{
    tdisk d;
    gpt_header h;

    (void)state;
    tdisk_init(&d, 512, 8192);
    gpt_disk(&d, stick, 3);
    assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 1, NULL, NULL, NULL),
                     GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_read_at(NULL, &d, 512, 1, &h, NULL, NULL),
                     GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 0, &h, NULL, NULL),
                     GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_read_at(tdisk_read, &d, 512, UINT64_MAX, &h, NULL,
                                 NULL), GPT_ERR_BAD_ARG);
    assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 1, &h, NULL, NULL),
                     GPT_OK);
    assert_int_equal(h.my_lba, 1);
    assert_int_equal(h.alt_lba, 8191);
    assert_memory_equal(h.disk_guid, disk_guid, 16);
    assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 8191, &h, NULL, NULL),
                     GPT_OK);
    assert_int_equal(h.alt_lba, 1);
    /* The primary read where the backup should be says it is elsewhere. */
    memcpy(blk(&d, 5), blk(&d, 1), 512);
    assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 5, &h, NULL, NULL),
                     GPT_ERR_NOT_GPT);
    /* A short array: three entries, 384 bytes, less than a block. */
    {
        uint8_t *hdr = blk(&d, 1);

        memcpy(hdr, blk(&d, 8191), 512);        /* a whole header again */
        put_le64(hdr + 24, 1);
        put_le64(hdr + 72, 2);
        gpt_copy(&d, 1, 8191, 2, stick, 3);
        put_le32(hdr + 80, 3);
        put_le32(hdr + 88, ref_crc32(blk(&d, 2), 384));
        put_le32(hdr + 16, 0);
        put_le32(hdr + 16, ref_crc32(hdr, 92));
        assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 1, &h, NULL, NULL),
                         GPT_OK);

        /* Entry sizes past a block, or not dividing it. */
        gpt_copy(&d, 1, 8191, 2, stick, 3);
        put_le32(hdr + 84, 1024);
        put_le32(hdr + 16, 0);
        put_le32(hdr + 16, ref_crc32(hdr, 92));
        assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 1, &h, NULL, NULL),
                         GPT_ERR_UNSUPPORTED);
        put_le32(hdr + 84, 384);
        put_le32(hdr + 16, 0);
        put_le32(hdr + 16, ref_crc32(hdr, 92));
        assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 1, &h, NULL, NULL),
                         GPT_ERR_UNSUPPORTED);
    }
    /* An entry array LBA no byte offset can hold. */
    put_le64(blk(&d, 1) + 72, UINT64_MAX / 512u);
    put_le32(blk(&d, 1) + 16, 0);
    put_le32(blk(&d, 1) + 16, ref_crc32(blk(&d, 1), 92));
    assert_int_equal(gpt_read_at(tdisk_read, &d, 512, 1, &h, NULL, NULL),
                     GPT_ERR_UNSUPPORTED);
    tdisk_free(&d);
}

/* ---- naming a disk ---- */

static void test_disk_id_text(void **state)
{
    part_disk_id a, b;
    char buf[PART_DISK_ID_TEXT];
    const char *g = "gpt:00112233-4455-6677-8899-aabbccddeeff";
    const char *G = "gpt:00112233-4455-6677-8899-AABBCCDDEEFF";

    (void)state;
    assert_int_equal(part_disk_id_parse(g, strlen(g), &a), PART_OK);
    assert_int_equal(a.kind, PART_KIND_GPT);
    /* The mixed-endian layout: the header's DiskGUID bytes. */
    assert_memory_equal(a.guid, disk_guid, 16);
    part_disk_id_format(&a, buf);
    assert_string_equal(buf, g);

    assert_int_equal(part_disk_id_parse(G, strlen(G), &b), PART_OK);
    assert_int_equal(part_disk_id_eq(&a, &b), 1);
    part_disk_id_format(&b, buf);
    assert_string_equal(buf, g);

    assert_int_equal(part_disk_id_parse("mbr:A1b2C3d4", 12, &a), PART_OK);
    assert_int_equal(a.kind, PART_KIND_MBR);
    assert_int_equal(a.mbr_sig, 0xA1B2C3D4u);
    part_disk_id_format(&a, buf);
    assert_string_equal(buf, "mbr:a1b2c3d4");
    assert_int_equal(part_disk_id_parse(buf, strlen(buf), &b), PART_OK);
    assert_int_equal(part_disk_id_eq(&a, &b), 1);

    /* Different kinds never match, nor does a missing side. */
    assert_int_equal(part_disk_id_parse(g, strlen(g), &b), PART_OK);
    assert_int_equal(part_disk_id_eq(&a, &b), 0);
    assert_int_equal(part_disk_id_eq(&a, NULL), 0);
    assert_int_equal(part_disk_id_eq(NULL, &a), 0);

    /* Signature 0 and an all-zero GUID name nothing. */
    assert_int_equal(part_disk_id_parse("mbr:00000000", 12, &a), PART_OK);
    assert_int_equal(part_disk_id_eq(&a, &a), 0);
    memset(&a, 0, sizeof(a));
    a.kind = PART_KIND_GPT;
    assert_int_equal(part_disk_id_eq(&a, &a), 0);
    b = a;
    b.guid[15] = 1;
    assert_int_equal(part_disk_id_eq(&a, &b), 0);
    assert_int_equal(part_disk_id_eq(&b, &b), 1);
    a.guid[15] = 2;
    assert_int_equal(part_disk_id_eq(&a, &b), 0);
    assert_int_equal(part_disk_id_parse("mbr:00000001", 12, &a), PART_OK);
    assert_int_equal(part_disk_id_parse("mbr:00000002", 12, &b), PART_OK);
    assert_int_equal(part_disk_id_eq(&a, &b), 0);

    /* No kind: nothing to match, nothing to print. */
    memset(&a, 0, sizeof(a));
    assert_int_equal(part_disk_id_eq(&a, &a), 0);
    strcpy(buf, "x");
    part_disk_id_format(&a, buf);
    assert_string_equal(buf, "");
    part_disk_id_format(NULL, buf);
    assert_string_equal(buf, "");
    part_disk_id_format(&a, NULL);

    /* The id a table read reports prints as STATION.INI spells it. */
    {
        tdisk d;
        ttable tt;

        tdisk_init(&d, 512, 1000);
        mbr_sig(&d, 0x0000BEEFu);
        mbr_entry(&d, 0, 0, 0x80, 0x07, 63, 100);
        tt_init(&tt, CAP);
        assert_int_equal(rd(&tt, &d, 1000), PART_OK);
        part_disk_id_format(&tt.t.id, buf);
        assert_string_equal(buf, "mbr:0000beef");
        tdisk_free(&d);
    }
}

static void test_disk_id_rejects(void **state)
{
    static const char *const bad[] = {
        "gpt:00112233-4455-6677-8899-aabbccddeef",     /* short */
        "gpt:00112233-4455-6677-8899-aabbccddeeff0",   /* long */
        "gpt:00112233_4455-6677-8899-aabbccddeeff",    /* separator */
        "gpt:00112233-4455_6677-8899-aabbccddeeff",
        "gpt:00112233-4455-6677_8899-aabbccddeeff",
        "gpt:00112233-4455-6677-8899_aabbccddeeff",
        "gpt:0011223g-4455-6677-8899-aabbccddeeff",    /* digit, each group */
        "gpt:00112233-445g-6677-8899-aabbccddeeff",
        "gpt:00112233-4455-667g-8899-aabbccddeeff",
        "gpt:00112233-4455-6677-889g-aabbccddeeff",
        "gpt:00112233-4455-6677-8899-aabbccddeefg",
        "GPT:00112233-4455-6677-8899-aabbccddeeff",    /* prefix */
        "mbr:1234567",
        "mbr:123456789",
        "mbr:1234567z",
        "mbr:1234567/",                                /* either side of */
        "mbr:1234567:",                                /* each digit range */
        "mbr:1234567@",
        "mbr:1234567G",
        "mbr:1234567`",
        "mbr 12345678",
        "xyz:12345678",
        "",
    };
    part_disk_id id;
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        id.kind = PART_KIND_MBR;
        assert_int_equal(part_disk_id_parse(bad[i], strlen(bad[i]), &id),
                         PART_ERR_ARG);
        assert_int_equal((int)id.kind, 0);
    }
    assert_int_equal(part_disk_id_parse(NULL, 0, &id), PART_ERR_ARG);
    assert_int_equal(part_disk_id_parse("mbr:12345678", 12, NULL),
                     PART_ERR_ARG);
}

int run_table_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_mbr_plain),
        cmocka_unit_test(test_mbr_extended_chain),
        cmocka_unit_test(test_mbr_chain_empty_first_ebr),
        cmocka_unit_test(test_mbr_looping_chain),
        cmocka_unit_test(test_mbr_escaping_chain),
        cmocka_unit_test(test_mbr_chain_bound),
        cmocka_unit_test(test_mbr_truncation),
        cmocka_unit_test(test_mbr_corrupt),
        cmocka_unit_test(test_no_table),
        cmocka_unit_test(test_gpt_protective),
        cmocka_unit_test(test_gpt_hybrid),
        cmocka_unit_test(test_gpt_backup),
        cmocka_unit_test(test_gpt_unsupported),
        cmocka_unit_test(test_gpt_corrupt_entries),
        cmocka_unit_test(test_4k_sectors),
        cmocka_unit_test(test_io_errors),
        cmocka_unit_test(test_bad_args),
        cmocka_unit_test(test_gpt_read_at_args),
        cmocka_unit_test(test_disk_id_text),
        cmocka_unit_test(test_disk_id_rejects),
    };

    return cmocka_run_group_tests_name("table", tests, NULL, NULL);
}
