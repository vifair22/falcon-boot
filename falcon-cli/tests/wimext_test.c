/* Tests for falcon-cli/src/sign/wimext.c, which answers the one question the
 * prepared-media shape of DESIGN Section 6.3 rests on: where in the ISO does
 * \sources\boot.wim begin, and is it one unbroken run there.
 *
 * A wrong answer is not caught later by anything: the loader serves the byte
 * range the manifest names, and Windows reads whatever that produces. So the
 * module's contract is that it refuses rather than guesses, and these tests
 * are mostly about the refusals.
 *
 * Everything here runs against synthetic in-memory images through the read
 * callback the module takes, which is the same seam falcon-core/src/fs/exfat.c
 * and falcon-core/src/luks use and exists for the same reason: no ISO, no
 * 7-Zip, no wimlib and no 7.7 GB file is needed to exercise the parts that
 * decide anything. The real media is covered by running the tool; what is
 * covered here is every path that leads to a refusal, which real media cannot
 * exercise at all because real media is well-formed.
 *
 * Three groups:
 *
 *   header     the 208-byte WIM header, including the containment check that
 *              ties a candidate to the length the ISO's directory entry
 *              claims. That check is what makes the scan's false-positive rate
 *              negligible, so its rejections are pinned one at a time.
 *
 *   locate     the scan: sector alignment is required, a decoy WIM earlier in
 *              the image is passed over because its head does not reproduce
 *              the extracted copy's, the search crosses the 1 MB chunk
 *              boundary, and "not there" is an error rather than an offset.
 *
 *   compare    the contiguity proof, and where it says the two sides parted.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include <sign/sign.h>
#include <sign/wimext.h>

/* ------------------------------------------------------------ fixtures */

typedef struct {
    const uint8_t *buf;
    uint64_t       len;
} mem_src;

static int mem_read(void *ctx, uint64_t off, void *out, size_t len,
                    size_t *got)
{
    const mem_src *m = ctx;
    size_t take;

    if (got != NULL)
        *got = 0;
    if (off >= m->len)
        return 0;                       /* past the end: zero bytes, not an error */
    take = (size_t)(m->len - off);
    if (take > len)
        take = len;
    memcpy(out, m->buf + off, take);
    if (got != NULL)
        *got = take;
    return 0;
}

/* A read callback that fails outright, for the SGN_ERR_IO paths. */
static int fail_read(void *ctx, uint64_t off, void *out, size_t len,
                     size_t *got)
{
    (void)ctx;
    (void)off;
    (void)out;
    (void)len;
    if (got != NULL)
        *got = 0;
    return 1;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    size_t i;

    for (i = 0; i < 4u; i++)
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    size_t i;

    for (i = 0; i < 8u; i++)
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
}

/* A resource header: 56-bit size, flag byte, offset, uncompressed size. */
static void put_reshdr(uint8_t *p, uint64_t size, uint64_t off)
{
    size_t i;

    for (i = 0; i < 7u; i++)
        p[i] = (uint8_t)((size >> (i * 8u)) & 0xFFu);
    p[7] = 0x02u;                       /* RESHDR_FLAG_COMPRESSED */
    put_le64(p + 8, off);
    put_le64(p + 16, size);
}

/* A structurally valid header for a WIM of exactly total_len bytes, laid out
 * the way Microsoft's own boot.wim is: the XML resource ends at the last byte
 * of the file, which is the property the containment check leans on. */
static void make_header(uint8_t *p, uint64_t total_len, uint32_t boot_index)
{
    memset(p, 0, WIM_HEADER_SIZE);
    memcpy(p, "MSWIM\0\0\0", 8);
    put_le32(p + 8, WIM_HEADER_SIZE);
    put_le32(p + 12, 0x00010D00u);      /* 1.13, as 25H2 media carries */
    put_le32(p + 16, 0x00040082u);
    put_le32(p + 20, 32768u);
    put_le16(p + 40, 1u);               /* part 1 */
    put_le16(p + 42, 1u);               /* of 1 */
    put_le32(p + 44, 2u);               /* two images, like real install media */
    put_reshdr(p + 48, 100u, total_len - 300u);   /* offset table */
    put_reshdr(p + 72, 200u, total_len - 200u);   /* XML, ending at EOF */
    put_reshdr(p + 96, 500u, 1000u);              /* boot metadata */
    put_le32(p + 120, boot_index);
    /* p + 124 is the integrity table: absent, so all zeroes. */
}

/* An image of image_len bytes carrying a wim_len-byte WIM at off, and
 * plausible noise everywhere else. The noise is deliberately not zeroes: a
 * scan that only ever ran over zeroed filler would not be tested at all. */
static uint8_t *make_image(uint64_t image_len, uint64_t off, uint64_t wim_len,
                           uint32_t boot_index)
{
    uint8_t *img = malloc((size_t)image_len);
    uint64_t i;

    assert_non_null(img);
    for (i = 0; i < image_len; i++)
        img[i] = (uint8_t)(i * 31u + 7u);
    if (wim_len > 0) {
        for (i = 0; i < wim_len; i++)
            img[off + i] = (uint8_t)(i * 13u + 3u);
        make_header(img + off, wim_len, boot_index);
    }
    return img;
}

/* ------------------------------------------------------------- header */

static void test_wimext_header_reads_the_fields(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8192u, &h), SGN_OK);
    assert_int_equal(h.header_size, WIM_HEADER_SIZE);
    assert_int_equal(h.part_number, 1);
    assert_int_equal(h.total_parts, 1);
    assert_int_equal(h.image_count, 2);
    /* The measured fact of DESIGN Section 6.3, read from the WIM rather than
     * hard-coded: image 2 is the one WinPE boots. */
    assert_int_equal(h.boot_index, 2);
    assert_int_equal(h.xml.offset + h.xml.size_in_wim, 8192u);
}

static void test_wimext_header_rejects_bad_magic(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    buf[4] = 'X';
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8192u, &h),
                     SGN_ERR_FORMAT);
}

static void test_wimext_header_rejects_wrong_header_size(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    put_le32(buf + 8, 192u);
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8192u, &h),
                     SGN_ERR_FORMAT);
}

static void test_wimext_header_rejects_short_buffer(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    assert_int_equal(wim_header_parse(buf, WIM_HEADER_SIZE - 1u, 8192u, &h),
                     SGN_ERR_FORMAT);
    assert_int_equal(wim_header_parse(NULL, sizeof(buf), 8192u, &h),
                     SGN_ERR_INTERNAL);
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8192u, NULL),
                     SGN_ERR_INTERNAL);
}

static void test_wimext_header_rejects_no_images(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    put_le32(buf + 44, 0u);
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8192u, &h),
                     SGN_ERR_FORMAT);
}

/* The check that ties a candidate to the ISO's directory entry: a header whose
 * resources run past the length the entry claims describes a different file
 * from the one we were told to find, whatever its magic says. */
static void test_wimext_header_rejects_resource_past_the_length(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8192u, &h), SGN_OK);
    /* One byte too long, and the same header is now rejected. */
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8191u, &h),
                     SGN_ERR_FORMAT);
}

static void test_wimext_header_rejects_resource_inside_itself(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    put_reshdr(buf + 96, 16u, 32u);     /* boot metadata inside the header */
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 8192u, &h),
                     SGN_ERR_FORMAT);
}

/* A caller that does not yet know the containing file's length still gets the
 * structural checks; it just cannot get the containment one. */
static void test_wimext_header_length_check_is_optional(void **state)
{
    uint8_t buf[WIM_HEADER_SIZE];
    wim_header h;

    (void)state;
    make_header(buf, 8192u, 2u);
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 0u, &h), SGN_OK);
    buf[0] = 'X';
    assert_int_equal(wim_header_parse(buf, sizeof(buf), 0u, &h),
                     SGN_ERR_FORMAT);
}

/* ------------------------------------------------------------- locate */

static void test_wimext_locate_finds_an_aligned_extent(void **state)
{
    const uint64_t image_len = 64u * 1024u;
    const uint64_t off = 8u * 2048u;
    const uint64_t wim_len = 8192u;
    uint8_t *img = make_image(image_len, off, wim_len, 2u);
    mem_src m;
    uint64_t found = 0;

    (void)state;
    m.buf = img;
    m.len = image_len;
    assert_int_equal(wimext_locate(mem_read, &m, image_len, img + off, 1024u,
                                   wim_len, &found), SGN_OK);
    assert_int_equal(found, off);
    free(img);
}

/* Data extents on this medium are sector-aligned and the override layer
 * addresses the virtual ISO the same way, so a WIM that begins off a boundary
 * is not found rather than found at an unusable offset. */
static void test_wimext_locate_ignores_an_unaligned_extent(void **state)
{
    const uint64_t image_len = 64u * 1024u;
    const uint64_t wim_len = 8192u;
    uint8_t *img = make_image(image_len, 8u * 2048u + 512u, wim_len, 2u);
    mem_src m;
    uint64_t found = 123;

    (void)state;
    m.buf = img;
    m.len = image_len;
    assert_int_equal(wimext_locate(mem_read, &m, image_len,
                                   img + 8u * 2048u + 512u, 1024u, wim_len,
                                   &found), SGN_ERR_FORMAT);
    assert_int_equal(found, 0);
    free(img);
}

/* A second WIM in the same image, of a different length, is what makes the
 * containment check earn its keep: install.wim sits beside boot.wim on real
 * media and carries the same magic at the same kind of boundary. */
static void test_wimext_locate_passes_over_a_decoy_wim(void **state)
{
    const uint64_t image_len = 128u * 1024u;
    const uint64_t decoy_off = 4u * 2048u;
    const uint64_t real_off = 20u * 2048u;
    const uint64_t wim_len = 8192u;
    uint8_t *img = make_image(image_len, real_off, wim_len, 2u);
    mem_src m;
    uint64_t found = 0;

    (void)state;
    /* The decoy is a perfectly valid WIM of the same length, planted earlier
     * in the image, so a scan that took the first structurally sound header
     * it saw would return it. Only the head comparison against the extracted
     * copy tells the two apart, which is exactly the install.wim case on real
     * media. */
    memcpy(img + decoy_off, img + real_off, (size_t)wim_len);
    put_le32(img + decoy_off + 120, 1u);
    m.buf = img;
    m.len = image_len;
    assert_int_equal(wimext_locate(mem_read, &m, image_len, img + real_off,
                                   1024u, wim_len, &found), SGN_OK);
    assert_int_equal(found, real_off);
    free(img);
}

/* The scan reads in 1 MB chunks; an extent past the first one has to be found
 * just the same, and the chunk boundary is exactly where an off-by-one would
 * hide. */
static void test_wimext_locate_crosses_the_chunk_boundary(void **state)
{
    const uint64_t image_len = 3u << 20;
    const uint64_t off = (2u << 20) + 4096u;
    const uint64_t wim_len = 8192u;
    uint8_t *img = make_image(image_len, off, wim_len, 2u);
    mem_src m;
    uint64_t found = 0;

    (void)state;
    m.buf = img;
    m.len = image_len;
    assert_int_equal(wimext_locate(mem_read, &m, image_len, img + off, 1024u,
                                   wim_len, &found), SGN_OK);
    assert_int_equal(found, off);
    free(img);
}

static void test_wimext_locate_refuses_when_absent(void **state)
{
    const uint64_t image_len = 64u * 1024u;
    uint8_t *img = make_image(image_len, 0, 0, 0);
    uint8_t head[WIM_HEADER_SIZE];
    mem_src m;
    uint64_t found = 0;

    (void)state;
    m.buf = img;
    m.len = image_len;
    make_header(head, 8192u, 2u);
    assert_int_equal(wimext_locate(mem_read, &m, image_len, head,
                                   sizeof(head), 8192u, &found),
                     SGN_ERR_FORMAT);
    free(img);
}

static void test_wimext_locate_argument_checks(void **state)
{
    mem_src m;
    uint64_t found = 0;
    uint8_t buf[16];
    uint8_t head[WIM_HEADER_SIZE];

    (void)state;
    memset(buf, 0, sizeof(buf));
    make_header(head, 8192u, 2u);
    m.buf = buf;
    m.len = sizeof(buf);
    assert_int_equal(wimext_locate(NULL, &m, 4096u, head, sizeof(head), 512u,
                                   &found), SGN_ERR_INTERNAL);
    assert_int_equal(wimext_locate(mem_read, &m, 4096u, head, sizeof(head),
                                   512u, NULL), SGN_ERR_INTERNAL);
    assert_int_equal(wimext_locate(mem_read, &m, 4096u, NULL, sizeof(head),
                                   512u, &found), SGN_ERR_INTERNAL);
    /* A needle shorter than a header, and one longer than the module will
     * hold, are both caller bugs rather than "not found". */
    assert_int_equal(wimext_locate(mem_read, &m, 4096u, head,
                                   WIM_HEADER_SIZE - 1u, 512u, &found),
                     SGN_ERR_INTERNAL);
    assert_int_equal(wimext_locate(mem_read, &m, 4096u, head,
                                   WIMEXT_HEAD_MAX + 1u, 512u, &found),
                     SGN_ERR_INTERNAL);
    /* A member longer than the image it is supposedly inside, and one shorter
     * than the needle it has to reproduce. */
    assert_int_equal(wimext_locate(mem_read, &m, 4096u, head, sizeof(head),
                                   8192u, &found), SGN_ERR_FORMAT);
    assert_int_equal(wimext_locate(mem_read, &m, 4096u, head, sizeof(head),
                                   8u, &found), SGN_ERR_FORMAT);
    assert_int_equal(wimext_locate(fail_read, &m, 4096u, head, sizeof(head),
                                   512u, &found), SGN_ERR_IO);
}

/* ------------------------------------------------------------ compare */

static void test_wimext_compare_identical(void **state)
{
    const uint64_t len = 300u * 1024u;   /* past one CMP_CHUNK, so it loops */
    uint8_t *a = make_image(len, 0, 0, 0);
    uint8_t *b = make_image(len, 0, 0, 0);
    mem_src ma;
    mem_src mb;
    uint64_t diff = 99;

    (void)state;
    ma.buf = a;
    ma.len = len;
    mb.buf = b;
    mb.len = len;
    assert_int_equal(wimext_compare(mem_read, &ma, 0, mem_read, &mb, 0, len,
                                    &diff), SGN_OK);
    assert_int_equal(diff, 0);
    free(a);
    free(b);
}

/* The same bytes at different offsets, which is the real shape: the ISO holds
 * the extent somewhere in the middle and the extracted copy starts at zero. */
static void test_wimext_compare_at_an_offset(void **state)
{
    const uint64_t len = 4096u;
    const uint64_t off = 10u * 2048u;
    uint8_t *img = make_image(64u * 1024u, 0, 0, 0);
    uint8_t *copy = malloc((size_t)len);
    mem_src ma;
    mem_src mb;

    (void)state;
    assert_non_null(copy);
    memcpy(copy, img + off, (size_t)len);
    ma.buf = img;
    ma.len = 64u * 1024u;
    mb.buf = copy;
    mb.len = len;
    assert_int_equal(wimext_compare(mem_read, &ma, off, mem_read, &mb, 0, len,
                                    NULL), SGN_OK);
    free(copy);
    free(img);
}

/* A fragmented file is a match that stops partway, so the offset at which it
 * stopped is the diagnostic worth having. */
static void test_wimext_compare_reports_the_first_difference(void **state)
{
    const uint64_t len = 4096u;
    uint8_t *a = make_image(len, 0, 0, 0);
    uint8_t *b = make_image(len, 0, 0, 0);
    mem_src ma;
    mem_src mb;
    uint64_t diff = 0;

    (void)state;
    b[1234] = (uint8_t)(b[1234] ^ 0xFFu);
    ma.buf = a;
    ma.len = len;
    mb.buf = b;
    mb.len = len;
    assert_int_equal(wimext_compare(mem_read, &ma, 0, mem_read, &mb, 0, len,
                                    &diff), SGN_ERR_FORMAT);
    assert_int_equal(diff, 1234);
    free(a);
    free(b);
}

static void test_wimext_compare_short_side_and_errors(void **state)
{
    const uint64_t len = 4096u;
    uint8_t *a = make_image(len, 0, 0, 0);
    mem_src ma;
    mem_src mb;
    uint64_t diff = 0;

    (void)state;
    ma.buf = a;
    ma.len = len;
    mb.buf = a;
    mb.len = 1000u;                     /* ends early */
    assert_int_equal(wimext_compare(mem_read, &ma, 0, mem_read, &mb, 0, len,
                                    &diff), SGN_ERR_FORMAT);
    assert_int_equal(diff, 1000);
    assert_int_equal(wimext_compare(fail_read, &ma, 0, mem_read, &ma, 0, len,
                                    &diff), SGN_ERR_IO);
    assert_int_equal(wimext_compare(NULL, &ma, 0, mem_read, &ma, 0, len,
                                    &diff), SGN_ERR_INTERNAL);
    /* A zero-length range is trivially equal and must not read anything. */
    assert_int_equal(wimext_compare(fail_read, &ma, 0, fail_read, &ma, 0, 0,
                                    &diff), SGN_OK);
    free(a);
}

/* ------------------------------------------------------------------ run */

int run_wimext_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_wimext_header_reads_the_fields),
        cmocka_unit_test(test_wimext_header_rejects_bad_magic),
        cmocka_unit_test(test_wimext_header_rejects_wrong_header_size),
        cmocka_unit_test(test_wimext_header_rejects_short_buffer),
        cmocka_unit_test(test_wimext_header_rejects_no_images),
        cmocka_unit_test(test_wimext_header_rejects_resource_past_the_length),
        cmocka_unit_test(test_wimext_header_rejects_resource_inside_itself),
        cmocka_unit_test(test_wimext_header_length_check_is_optional),
        cmocka_unit_test(test_wimext_locate_finds_an_aligned_extent),
        cmocka_unit_test(test_wimext_locate_ignores_an_unaligned_extent),
        cmocka_unit_test(test_wimext_locate_passes_over_a_decoy_wim),
        cmocka_unit_test(test_wimext_locate_crosses_the_chunk_boundary),
        cmocka_unit_test(test_wimext_locate_refuses_when_absent),
        cmocka_unit_test(test_wimext_locate_argument_checks),
        cmocka_unit_test(test_wimext_compare_identical),
        cmocka_unit_test(test_wimext_compare_at_an_offset),
        cmocka_unit_test(test_wimext_compare_reports_the_first_difference),
        cmocka_unit_test(test_wimext_compare_short_side_and_errors),
    };

    return cmocka_run_group_tests_name("WIM extent location (bless tool)",
                                       tests, NULL, NULL);
}
