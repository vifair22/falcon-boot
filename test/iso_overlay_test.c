/* Tests for src/iso/overlay.h, the address arithmetic of the virtual-media
 * override layer (docs/DESIGN.md Section 6.3).
 *
 * Why this group exists at all. src/iso/iso.c is firmware all the way down -
 * Block I/O, device paths, ConnectController - and cannot run in a host
 * process. But the part of it that can corrupt a medium silently is not the
 * firmware plumbing: it is the arithmetic that decides which bytes of a block
 * read come from the ISO on p2 and which come from a sidecar. Get that wrong
 * by one byte and the firmware reports nothing, the ISO mounts, and Windows
 * Setup reads a WIM with a hole in it. So the arithmetic is split out into an
 * EFI-free header and pinned here, the same way src/bless/blesscore.c is split
 * out from the firmware half it serves.
 *
 * The properties worth pinning, in the order they can bite:
 *
 *   splitting     a block is 2048 bytes and an override is an arbitrary byte
 *                 range; nothing aligns the two. Every case where a request
 *                 crosses a boundary - into an override, out of one, over a
 *                 whole one, across two back to back - has to cut exactly at
 *                 the boundary and route each piece to the right source at
 *                 the right offset in that source's OWN address space.
 *
 *   coverage      the pieces must tile the request exactly: ascending,
 *                 contiguous, no gap and no overlap, summing to the request
 *                 length. A gap is a hole of stale buffer; an overlap is a
 *                 double write. Both are checked structurally on every split
 *                 in this file rather than case by case.
 *
 *   the spaces    virtual, image and sidecar are three different address
 *                 spaces (overlay.h). Bounds are decided in the virtual one,
 *                 which is the one an append region makes bigger than the
 *                 file. A check written against the file's size looks right
 *                 and is wrong only for prepared media.
 *
 *   fail-closed   the set arrives from a writable exFAT partition that anyone
 *                 can edit. Unsorted, overlapping, oversized, zero-length,
 *                 wrapping, past the end, or promising more bytes than the
 *                 sidecar holds: every one of those is a refusal, and the
 *                 refusal has to name which one it was.
 *
 * What is deliberately NOT here: reading anything. The split never
 * dereferences an fs_file, so the sidecar handles below are address-distinct
 * dummies and no volume is mounted. Proving that the bytes arrive is the rig's
 * job (DESIGN Section 11 step 11), and no host process can stand in for it.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <iso/overlay.h>

/* Two sidecars, used only as distinct addresses: the split routes pointers and
 * never follows them. Static rather than on a stack because an fs_file carries
 * a driver's whole context (4 KiB of sector cache) and three of them would be a
 * silly frame for a test that reads none of it. */
static fs_file side_a;
static fs_file side_b;

#define CD_BLOCK 2048u

/* Builds a set from a compact literal table, so each test reads as the map it
 * is testing rather than as ten lines of struct assignment. */
typedef struct {
    uint64_t iso_off;
    uint64_t len;
    fs_file *src;
    uint64_t src_off;
} ovl_row;

static void build(iso_overlay *o, const ovl_row *rows, size_t n, uint64_t extra)
{
    size_t i;

    memset(o, 0, sizeof(*o));
    o->n = n;
    o->extra = extra;
    for (i = 0; i < n && i < ISO_MAX_OVERRIDES; i++) {
        o->ovl[i].iso_off = rows[i].iso_off;
        o->ovl[i].len = rows[i].len;
        o->ovl[i].src = rows[i].src;
        o->ovl[i].src_off = rows[i].src_off;
    }
}

/* Every split in this file goes through here, so the tiling property is
 * asserted on every case rather than only where someone remembered to. A
 * segment list that has a gap, an overlap, a zero-length piece or the wrong
 * total is wrong no matter what the individual routing assertions say. */
static void check_tiling(const iso_segment *seg, size_t n, uint64_t off,
                         uint64_t len)
{
    uint64_t cur = off;
    size_t i;

    for (i = 0; i < n; i++) {
        assert_int_equal(seg[i].virt_off, cur);
        assert_true(seg[i].len > 0);
        cur += seg[i].len;
    }
    assert_int_equal(cur, off + len);
}

static size_t split_ok(const iso_overlay *o, uint64_t media, uint64_t off,
                       uint64_t len, iso_segment *seg)
{
    size_t n = 12345; /* poisoned: a failing call must still zero it */

    assert_int_equal(iso_overlay_split(o, media, off, len, seg,
                                       ISO_MAX_SEGMENTS, &n), ISO_OVL_OK);
    check_tiling(seg, n, off, len);
    return n;
}

/* ---- the split ---- */

/* No overrides at all: one segment, straight from the image, and the image's
 * offset equals the virtual one. This is the shape the ordinary boot path
 * takes on every medium on the stick. */
static void test_split_no_overrides(void **state)
{
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    build(&o, NULL, 0, 0);
    n = split_ok(&o, 1u << 20, 4096, CD_BLOCK, seg);
    assert_int_equal(n, 1);
    assert_null(seg[0].src);
    assert_int_equal(seg[0].src_off, 4096);
    assert_int_equal(seg[0].len, CD_BLOCK);
}

/* A block read that starts before an override and runs into it. The cut lands
 * on the override's first byte, and the sidecar read starts at its own base
 * offset, not at the virtual one. */
static void test_split_straddles_start(void **state)
{
    static const ovl_row rows[] = { { 5000, 3000, &side_a, 100000 } };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    build(&o, rows, 1, 0);
    /* Block 2 of a CD: bytes 4096..6143, so 904 image bytes then 1144 sidecar
     * bytes. */
    n = split_ok(&o, 1u << 20, 4096, CD_BLOCK, seg);
    assert_int_equal(n, 2);
    assert_null(seg[0].src);
    assert_int_equal(seg[0].src_off, 4096);
    assert_int_equal(seg[0].len, 904);
    assert_ptr_equal(seg[1].src, &side_a);
    assert_int_equal(seg[1].virt_off, 5000);
    assert_int_equal(seg[1].src_off, 100000);
    assert_int_equal(seg[1].len, 1144);
}

/* The mirror case: a read that starts INSIDE an override and runs out of it.
 * The sidecar offset has to be advanced by how far in the cursor already was,
 * which is the single easiest byte to drop. */
static void test_split_straddles_end(void **state)
{
    static const ovl_row rows[] = { { 5000, 3000, &side_a, 100000 } };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    /* Bytes 6144..8191: 1856 inside the override (which ends at 8000), then
     * 192 back from the image. */
    build(&o, rows, 1, 0);
    n = split_ok(&o, 1u << 20, 6144, CD_BLOCK, seg);
    assert_int_equal(n, 2);
    assert_ptr_equal(seg[0].src, &side_a);
    assert_int_equal(seg[0].src_off, 100000 + (6144 - 5000));
    assert_int_equal(seg[0].len, 8000 - 6144);
    assert_null(seg[1].src);
    assert_int_equal(seg[1].virt_off, 8000);
    assert_int_equal(seg[1].src_off, 8000);
    assert_int_equal(seg[1].len, 192);
}

/* Wholly inside one override: one segment, no image read at all. */
static void test_split_inside(void **state)
{
    static const ovl_row rows[] = { { 4096, 65536, &side_a, 0 } };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    build(&o, rows, 1, 0);
    n = split_ok(&o, 1u << 20, 8192, CD_BLOCK, seg);
    assert_int_equal(n, 1);
    assert_ptr_equal(seg[0].src, &side_a);
    assert_int_equal(seg[0].src_off, 8192 - 4096);
    assert_int_equal(seg[0].len, CD_BLOCK);
}

/* Exact boundaries, both ends. An override that begins exactly where the read
 * begins and ends exactly where it ends must produce ONE segment, not three
 * with two empty ones, and must not fall into the "before the next override"
 * branch by an off-by-one in the <= comparisons. */
static void test_split_exact_boundaries(void **state)
{
    static const ovl_row rows[] = { { 4096, CD_BLOCK, &side_a, 512 } };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    build(&o, rows, 1, 0);
    n = split_ok(&o, 1u << 20, 4096, CD_BLOCK, seg);
    assert_int_equal(n, 1);
    assert_ptr_equal(seg[0].src, &side_a);
    assert_int_equal(seg[0].src_off, 512);

    /* The block just before it is pure image, and the one just after too:
     * neither may be pulled into the override by a boundary that is inclusive
     * on the wrong side. */
    n = split_ok(&o, 1u << 20, 2048, CD_BLOCK, seg);
    assert_int_equal(n, 1);
    assert_null(seg[0].src);
    n = split_ok(&o, 1u << 20, 6144, CD_BLOCK, seg);
    assert_int_equal(n, 1);
    assert_null(seg[0].src);
}

/* One read spanning two overrides with image bytes between them: the worst
 * ordinary case, five pieces from three sources. */
static void test_split_spans_two(void **state)
{
    static const ovl_row rows[] = {
        { 1000, 500, &side_a, 0 },
        { 2000, 500, &side_b, 7 },
    };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    build(&o, rows, 2, 0);
    n = split_ok(&o, 1u << 20, 0, 4096, seg);
    assert_int_equal(n, 5);
    assert_null(seg[0].src);
    assert_int_equal(seg[0].len, 1000);
    assert_ptr_equal(seg[1].src, &side_a);
    assert_int_equal(seg[1].src_off, 0);
    assert_int_equal(seg[1].len, 500);
    assert_null(seg[2].src);
    assert_int_equal(seg[2].virt_off, 1500);
    assert_int_equal(seg[2].src_off, 1500);
    assert_int_equal(seg[2].len, 500);
    assert_ptr_equal(seg[3].src, &side_b);
    assert_int_equal(seg[3].src_off, 7);
    assert_int_equal(seg[3].len, 500);
    assert_null(seg[4].src);
    assert_int_equal(seg[4].virt_off, 2500);
    assert_int_equal(seg[4].len, 4096 - 2500);
}

/* Two overrides that touch, from different sidecars. Legal (check allows
 * prev_end == iso_off) and must stay two segments with two sources: merging
 * them would read the second range out of the first sidecar. */
static void test_split_adjacent_overrides(void **state)
{
    static const ovl_row rows[] = {
        { 1024, 1024, &side_a, 0 },
        { 2048, 1024, &side_b, 0 },
    };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    build(&o, rows, 2, 0);
    n = split_ok(&o, 1u << 20, 1024, 2048, seg);
    assert_int_equal(n, 2);
    assert_ptr_equal(seg[0].src, &side_a);
    assert_ptr_equal(seg[1].src, &side_b);
    assert_int_equal(seg[1].virt_off, 2048);
}

/* The maximum set, read end to end in one request: the worst case the segment
 * bound was computed for. ISO_MAX_SEGMENTS must actually hold it. */
static void test_split_worst_case_fits(void **state)
{
    static const ovl_row rows[] = {
        { 100, 10, &side_a, 0 },
        { 200, 10, &side_b, 0 },
        { 300, 10, &side_a, 20 },
        { 400, 10, &side_b, 20 },
    };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    assert_int_equal(ISO_MAX_OVERRIDES, 4);
    build(&o, rows, 4, 0);
    /* Starts before the first and ends after the last: four overrides and five
     * image gaps. */
    n = split_ok(&o, 1u << 20, 0, 1000, seg);
    assert_int_equal(n, 9);
    assert_int_equal(n, ISO_MAX_SEGMENTS);
}

/* A cap smaller than the cut needs is refused rather than truncated: a short
 * segment list would look like a successful read of a shorter request. */
static void test_split_nospace(void **state)
{
    static const ovl_row rows[] = { { 1000, 500, &side_a, 0 } };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n = 7;

    (void)state;
    build(&o, rows, 1, 0);
    assert_int_equal(iso_overlay_split(&o, 1u << 20, 0, 4096, seg, 2, &n),
                     ISO_OVL_ERR_NOSPACE);
    /* Two segments would have fit; the third is where it gives up. */
    assert_int_equal(iso_overlay_split(&o, 1u << 20, 0, 4096, seg, 3, &n),
                     ISO_OVL_OK);
    assert_int_equal(n, 3);
}

/* A zero-length request is success with nothing to do. The Block I/O caller
 * has already answered EFI_SUCCESS for that case; making it an error here
 * would put two different opinions in the same read path. */
static void test_split_zero_length(void **state)
{
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n = 99;

    (void)state;
    build(&o, NULL, 0, 0);
    assert_int_equal(iso_overlay_split(&o, 4096, 0, 0, seg, ISO_MAX_SEGMENTS,
                                       &n), ISO_OVL_OK);
    assert_int_equal(n, 0);
}

/* Bounds are the VIRTUAL medium's, and they are hard. The last block is fine,
 * one byte past it is not, and a request whose start is inside but whose end
 * is outside is not either. */
static void test_split_past_media_end(void **state)
{
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n = 5;

    (void)state;
    build(&o, NULL, 0, 0);
    assert_int_equal(iso_overlay_split(&o, 8192, 6144, CD_BLOCK, seg,
                                       ISO_MAX_SEGMENTS, &n), ISO_OVL_OK);
    assert_int_equal(iso_overlay_split(&o, 8192, 6145, CD_BLOCK, seg,
                                       ISO_MAX_SEGMENTS, &n),
                     ISO_OVL_ERR_RANGE);
    assert_int_equal(n, 0);
    assert_int_equal(iso_overlay_split(&o, 8192, 8192, 1, seg,
                                       ISO_MAX_SEGMENTS, &n),
                     ISO_OVL_ERR_RANGE);
    /* A request that wraps uint64 must not be arithmetic'd into range. */
    assert_int_equal(iso_overlay_split(&o, 8192, UINT64_MAX - 3, 8, seg,
                                       ISO_MAX_SEGMENTS, &n),
                     ISO_OVL_ERR_BAD_ARG);
}

/* An append region is virtual medium the image file does not back, and the
 * split neither knows nor needs the file's size: an override sitting entirely
 * past it is an ordinary entry, and uncovered appended bytes fall through to
 * the image branch, where the read path turns them into zeros. */
static void test_split_append_region(void **state)
{
    /* Image is 8192 bytes, medium is 16384, the appended half backed from a
     * sidecar. */
    static const ovl_row rows[] = { { 8192, 4096, &side_a, 1000 } };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n;

    (void)state;
    build(&o, rows, 1, 8192);
    n = split_ok(&o, 16384, 8192, CD_BLOCK, seg);
    assert_int_equal(n, 1);
    assert_ptr_equal(seg[0].src, &side_a);
    assert_int_equal(seg[0].src_off, 1000);

    /* The appended tail nobody backed: still the image branch, still in range,
     * and src_off is the virtual offset past the file's end. The read path
     * clamps that to zeros; the arithmetic must hand it over rather than
     * refuse it. */
    n = split_ok(&o, 16384, 12288, CD_BLOCK, seg);
    assert_int_equal(n, 1);
    assert_null(seg[0].src);
    assert_int_equal(seg[0].src_off, 12288);

    /* And the whole appended half in one request, crossing from backed to
     * unbacked. */
    n = split_ok(&o, 16384, 8192, 8192, seg);
    assert_int_equal(n, 2);
    assert_ptr_equal(seg[0].src, &side_a);
    assert_int_equal(seg[0].len, 4096);
    assert_null(seg[1].src);
    assert_int_equal(seg[1].len, 4096);
}

static void test_split_null_args(void **state)
{
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    size_t n = 3;

    (void)state;
    build(&o, NULL, 0, 0);
    assert_int_equal(iso_overlay_split(NULL, 4096, 0, 512, seg,
                                       ISO_MAX_SEGMENTS, &n),
                     ISO_OVL_ERR_BAD_ARG);
    assert_int_equal(iso_overlay_split(&o, 4096, 0, 512, NULL,
                                       ISO_MAX_SEGMENTS, &n),
                     ISO_OVL_ERR_BAD_ARG);
    assert_int_equal(iso_overlay_split(&o, 4096, 0, 512, seg,
                                       ISO_MAX_SEGMENTS, NULL),
                     ISO_OVL_ERR_BAD_ARG);
    /* A count-only failure still leaves the caller's count zeroed, so a
     * caller that ignores the status cannot loop over stale segments. */
    o.n = ISO_MAX_OVERRIDES + 1u;
    assert_int_equal(iso_overlay_split(&o, 4096, 0, 512, seg,
                                       ISO_MAX_SEGMENTS, &n),
                     ISO_OVL_ERR_COUNT);
    assert_int_equal(n, 0);
}

/* ---- validation ---- */

static void test_check_accepts(void **state)
{
    static const ovl_row rows[] = {
        { 1000, 500, &side_a, 0 },
        { 1500, 500, &side_b, 4096 },
    };
    static const uint64_t sizes[] = { 500, 4596 };
    iso_overlay o;

    (void)state;
    build(&o, rows, 2, 0);
    /* Adjacent ranges, sidecars exactly the size their entries need: the
     * boundary case on both counts, and it is accepted. */
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes), ISO_OVL_OK);
}

static void test_check_empty_set(void **state)
{
    iso_overlay o;

    (void)state;
    build(&o, NULL, 0, 0);
    /* No entries means no sidecar sizes to supply. */
    assert_int_equal(iso_overlay_check(&o, 1u << 20, NULL), ISO_OVL_OK);
    assert_int_equal(iso_overlay_check(NULL, 1u << 20, NULL),
                     ISO_OVL_ERR_BAD_ARG);
}

static void test_check_over_bound(void **state)
{
    static const uint64_t sizes[ISO_MAX_OVERRIDES] = { 0 };
    iso_overlay o;

    (void)state;
    build(&o, NULL, 0, 0);
    o.n = ISO_MAX_OVERRIDES + 1u;
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes),
                     ISO_OVL_ERR_COUNT);
}

static void test_check_zero_length(void **state)
{
    static const ovl_row rows[] = { { 1000, 0, &side_a, 0 } };
    static const uint64_t sizes[] = { 4096 };
    iso_overlay o;

    (void)state;
    build(&o, rows, 1, 0);
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes),
                     ISO_OVL_ERR_EMPTY);
}

static void test_check_unsorted(void **state)
{
    static const ovl_row rows[] = {
        { 2000, 500, &side_a, 0 },
        { 1000, 500, &side_b, 0 },
    };
    static const uint64_t sizes[] = { 4096, 4096 };
    iso_overlay o;

    (void)state;
    build(&o, rows, 2, 0);
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes),
                     ISO_OVL_ERR_ORDER);
}

static void test_check_overlapping(void **state)
{
    static const ovl_row rows[] = {
        { 1000, 500, &side_a, 0 },
        { 1499, 500, &side_b, 0 },
    };
    static const ovl_row same[] = {
        { 1000, 500, &side_a, 0 },
        { 1000, 10, &side_b, 0 },
    };
    static const uint64_t sizes[] = { 4096, 4096 };
    iso_overlay o;

    (void)state;
    /* Overlap by one byte: the cheapest way for a hand-edited map to make
     * "which entry answers here" ambiguous. */
    build(&o, rows, 2, 0);
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes),
                     ISO_OVL_ERR_ORDER);
    build(&o, same, 2, 0);
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes),
                     ISO_OVL_ERR_ORDER);
}

static void test_check_past_media(void **state)
{
    static const ovl_row rows[] = { { 8000, 1000, &side_a, 0 } };
    static const uint64_t sizes[] = { 1u << 20 };
    iso_overlay o;

    (void)state;
    build(&o, rows, 1, 0);
    assert_int_equal(iso_overlay_check(&o, 8192, sizes), ISO_OVL_ERR_RANGE);
    /* Ending exactly at the last byte is in range. */
    assert_int_equal(iso_overlay_check(&o, 9000, sizes), ISO_OVL_OK);
}

static void test_check_wrapping(void **state)
{
    static const ovl_row virt[] = { { UINT64_MAX - 3, 8, &side_a, 0 } };
    static const ovl_row side[] = { { 0, 8, &side_a, UINT64_MAX - 3 } };
    static const uint64_t sizes[] = { UINT64_MAX };
    iso_overlay o;

    (void)state;
    /* Neither space may be wrapped into range: an entry that wraps would pass
     * a naive "start + len <= media" check with room to spare. */
    build(&o, virt, 1, 0);
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes),
                     ISO_OVL_ERR_RANGE);
    build(&o, side, 1, 0);
    assert_int_equal(iso_overlay_check(&o, 1u << 20, sizes),
                     ISO_OVL_ERR_SOURCE);
}

static void test_check_source(void **state)
{
    static const ovl_row rows[] = { { 1000, 500, &side_a, 4096 } };
    static const uint64_t exact[] = { 4596 };
    static const uint64_t one_short[] = { 4595 };
    iso_overlay o;

    (void)state;
    build(&o, rows, 1, 0);
    assert_int_equal(iso_overlay_check(&o, 1u << 20, exact), ISO_OVL_OK);
    /* One byte short is the whole point: a sidecar that nearly covers its
     * entry would serve a block of stale buffer at the very end of the
     * substituted range, which is exactly where nobody looks. */
    assert_int_equal(iso_overlay_check(&o, 1u << 20, one_short),
                     ISO_OVL_ERR_SOURCE);
    o.ovl[0].src = NULL;
    assert_int_equal(iso_overlay_check(&o, 1u << 20, exact),
                     ISO_OVL_ERR_SOURCE);
    o.ovl[0].src = &side_a;
    assert_int_equal(iso_overlay_check(&o, 1u << 20, NULL),
                     ISO_OVL_ERR_BAD_ARG);
}

/* ---- media geometry ---- */

static void test_media_size(void **state)
{
    iso_overlay o;
    uint64_t out = 1;

    (void)state;
    /* No append region: whole blocks, and a partial tail is truncated away
     * exactly as the no-overlay path has always done. */
    build(&o, NULL, 0, 0);
    assert_int_equal(iso_overlay_media_size(&o, 8192, CD_BLOCK, &out),
                     ISO_OVL_OK);
    assert_int_equal(out, 8192);
    assert_int_equal(iso_overlay_media_size(&o, 8192 + 5, CD_BLOCK, &out),
                     ISO_OVL_OK);
    assert_int_equal(out, 8192);
    /* A NULL overlay is the ordinary medium and must answer the same. */
    assert_int_equal(iso_overlay_media_size(NULL, 8192 + 5, CD_BLOCK, &out),
                     ISO_OVL_OK);
    assert_int_equal(out, 8192);

    /* With one: the medium is larger than the file, which is the whole
     * property the read path's bounds arithmetic has to survive. */
    build(&o, NULL, 0, 4096);
    assert_int_equal(iso_overlay_media_size(&o, 8192, CD_BLOCK, &out),
                     ISO_OVL_OK);
    assert_int_equal(out, 12288);

    /* An append region whose total is not whole blocks is refused rather than
     * silently shortened: the preparer computed a region the device cannot
     * advertise. */
    assert_int_equal(iso_overlay_media_size(&o, 8192 + 5, CD_BLOCK, &out),
                     ISO_OVL_ERR_GEOM);
    assert_int_equal(out, 0);

    build(&o, NULL, 0, UINT64_MAX - 3);
    assert_int_equal(iso_overlay_media_size(&o, 8192, CD_BLOCK, &out),
                     ISO_OVL_ERR_RANGE);

    build(&o, NULL, 0, 0);
    assert_int_equal(iso_overlay_media_size(&o, 100, CD_BLOCK, &out),
                     ISO_OVL_ERR_RANGE);
    assert_int_equal(iso_overlay_media_size(&o, 8192, 0, &out),
                     ISO_OVL_ERR_BAD_ARG);
    assert_int_equal(iso_overlay_media_size(&o, 8192, CD_BLOCK, NULL),
                     ISO_OVL_ERR_BAD_ARG);
}

/* Every 2048-byte block of a small medium, read one at a time, reassembles
 * into exactly the byte map the set describes. This is the property the
 * firmware actually exercises - it streams the medium block by block - and it
 * is the one a hand-built case list can miss: the model here is recomputed
 * per byte from the set, independently of the split's own walk. */
static void test_split_block_sweep(void **state)
{
    static const ovl_row rows[] = {
        { 700, 1500, &side_a, 64 },
        { 2200, 100, &side_b, 0 },
        { 6000, 2000, &side_a, 9000 },
    };
    iso_overlay o;
    iso_segment seg[ISO_MAX_SEGMENTS];
    uint64_t media = 12288;
    uint64_t lba;

    (void)state;
    build(&o, rows, 3, 0);
    for (lba = 0; lba * CD_BLOCK < media; lba++) {
        uint64_t off = lba * CD_BLOCK;
        size_t n = split_ok(&o, media, off, CD_BLOCK, seg);
        size_t i;
        uint64_t b;

        for (i = 0; i < n; i++) {
            for (b = 0; b < seg[i].len; b++) {
                uint64_t virt = seg[i].virt_off + b;
                fs_file *want_src = NULL;
                uint64_t want_off = virt;
                size_t k;

                for (k = 0; k < o.n; k++) {
                    if (virt >= o.ovl[k].iso_off &&
                        virt < o.ovl[k].iso_off + o.ovl[k].len) {
                        want_src = o.ovl[k].src;
                        want_off = o.ovl[k].src_off +
                                   (virt - o.ovl[k].iso_off);
                        break;
                    }
                }
                assert_ptr_equal(seg[i].src, want_src);
                assert_int_equal(seg[i].src_off + b, want_off);
            }
        }
    }
}

int run_iso_overlay_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_split_no_overrides),
        cmocka_unit_test(test_split_straddles_start),
        cmocka_unit_test(test_split_straddles_end),
        cmocka_unit_test(test_split_inside),
        cmocka_unit_test(test_split_exact_boundaries),
        cmocka_unit_test(test_split_spans_two),
        cmocka_unit_test(test_split_adjacent_overrides),
        cmocka_unit_test(test_split_worst_case_fits),
        cmocka_unit_test(test_split_nospace),
        cmocka_unit_test(test_split_zero_length),
        cmocka_unit_test(test_split_past_media_end),
        cmocka_unit_test(test_split_append_region),
        cmocka_unit_test(test_split_null_args),
        cmocka_unit_test(test_split_block_sweep),
        cmocka_unit_test(test_check_accepts),
        cmocka_unit_test(test_check_empty_set),
        cmocka_unit_test(test_check_over_bound),
        cmocka_unit_test(test_check_zero_length),
        cmocka_unit_test(test_check_unsorted),
        cmocka_unit_test(test_check_overlapping),
        cmocka_unit_test(test_check_past_media),
        cmocka_unit_test(test_check_wrapping),
        cmocka_unit_test(test_check_source),
        cmocka_unit_test(test_media_size),
    };

    return cmocka_run_group_tests_name("iso overlay", tests, NULL, NULL);
}
