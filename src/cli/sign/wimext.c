/* WIM extent location. See wimext.h for why this is a scan plus a full-range
 * comparison rather than a UDF reader.
 */

#include <cli/sign/sign.h>
#include <cli/sign/wimext.h>

#include <stdlib.h>
#include <string.h>

/* Little-endian loads are open-coded here rather than pulled from
 * src/lib/string.h: that header exists for the freestanding build and also
 * declares the mem* routines the host gets from libc, and this file has no
 * other reason to care. Four short functions cost less than the collision. */
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* One resource header: seven bytes of compressed size, one flag byte, then
 * two 64-bit fields. The 56-bit size is why this is not a plain le64. */
static void read_reshdr(const uint8_t *p, wim_reshdr *r)
{
    uint64_t i;

    r->size_in_wim = 0;
    for (i = 0; i < 7u; i++)
        r->size_in_wim |= (uint64_t)p[i] << (i * 8u);
    r->flags = p[7];
    r->offset = le64(p + 8);
    r->uncompressed = le64(p + 16);
}

static const uint8_t k_magic[8] = { 'M', 'S', 'W', 'I', 'M', 0, 0, 0 };

/* A resource must either be absent (all zeroes) or lie wholly inside the file
 * and start past the header. Both halves matter: a resource overlapping the
 * header is a corrupt WIM, and one running past the end is the signature of a
 * coincidental magic match somewhere in the middle of a compressed payload. */
static int reshdr_in_range(const wim_reshdr *r, uint64_t total_len,
                           const char *what)
{
    if (r->size_in_wim == 0 && r->offset == 0)
        return SGN_OK;
    if (r->offset < WIM_HEADER_SIZE) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "WIM header's %s resource starts at %llu, inside the "
                      "header itself", what, (unsigned long long)r->offset);
        return SGN_ERR_FORMAT;
    }
    if (r->offset > total_len || r->size_in_wim > total_len - r->offset) {
        uint64_t end = r->offset + r->size_in_wim;

        sgn_set_error(SGN_ERR_FORMAT,
                      "WIM header's %s resource runs to %llu, past the %llu "
                      "bytes the ISO's directory entry allows",
                      what, (unsigned long long)end,
                      (unsigned long long)total_len);
        return SGN_ERR_FORMAT;
    }
    return SGN_OK;
}

int wim_header_parse(const void *buf, size_t len, uint64_t total_len,
                     wim_header *out)
{
    const uint8_t *p = buf;
    wim_header h;
    size_t i;
    int rc;

    if (buf == NULL || out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "wim_header_parse: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    if (len < WIM_HEADER_SIZE) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "only %zu bytes available where a %u-byte WIM header "
                      "was expected", len, (unsigned)WIM_HEADER_SIZE);
        return SGN_ERR_FORMAT;
    }
    for (i = 0; i < sizeof(k_magic); i++) {
        if (p[i] != k_magic[i]) {
            sgn_set_error(SGN_ERR_FORMAT, "no MSWIM magic");
            return SGN_ERR_FORMAT;
        }
    }

    h.header_size   = le32(p + 8);
    h.version       = le32(p + 12);
    h.flags         = le32(p + 16);
    h.chunk_size    = le32(p + 20);
    /* p + 24 is the 16-byte volume GUID; nothing here needs it. */
    h.part_number   = le16(p + 40);
    h.total_parts   = le16(p + 42);
    h.image_count   = le32(p + 44);
    read_reshdr(p + 48,  &h.offset_table);
    read_reshdr(p + 72,  &h.xml);
    read_reshdr(p + 96,  &h.boot_metadata);
    h.boot_index    = le32(p + 120);
    read_reshdr(p + 124, &h.integrity);

    if (h.header_size != WIM_HEADER_SIZE) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "WIM header claims %u bytes, not %u",
                      (unsigned)h.header_size, (unsigned)WIM_HEADER_SIZE);
        return SGN_ERR_FORMAT;
    }
    /* One image is the minimum a WIM can carry and install media carries two.
     * The upper bound is a sanity rail for the scan, not a format limit. */
    if (h.image_count == 0 || h.image_count > 1024u) {
        sgn_set_error(SGN_ERR_FORMAT, "WIM header claims %u images",
                      (unsigned)h.image_count);
        return SGN_ERR_FORMAT;
    }

    if (total_len != 0) {
        rc = reshdr_in_range(&h.offset_table, total_len, "offset-table");
        if (rc != SGN_OK)
            return rc;
        rc = reshdr_in_range(&h.xml, total_len, "XML");
        if (rc != SGN_OK)
            return rc;
        rc = reshdr_in_range(&h.boot_metadata, total_len, "boot-metadata");
        if (rc != SGN_OK)
            return rc;
        rc = reshdr_in_range(&h.integrity, total_len, "integrity");
        if (rc != SGN_OK)
            return rc;
    }

    *out = h;
    return SGN_OK;
}

/* ------------------------------------------------------------ the scan */

/* One megabyte, and a multiple of WIMEXT_ALIGN so that every candidate offset
 * in a chunk has its whole 8-byte magic inside the same chunk. Static because
 * the tool is single-threaded by construction and a megabyte on the stack is
 * not a thing this repo does anywhere. */
#define SCAN_CHUNK (1u << 20)

static uint8_t g_scan[SCAN_CHUNK];
static uint8_t g_head[WIMEXT_HEAD_MAX];

int wimext_locate(wimext_read_fn rd, void *ctx, uint64_t image_size,
                  const void *want_head, size_t head_len,
                  uint64_t want_len, uint64_t *out_off)
{
    uint64_t base = 0;

    if (rd == NULL || out_off == NULL || want_head == NULL ||
        head_len < WIM_HEADER_SIZE || head_len > sizeof(g_head)) {
        sgn_set_error(SGN_ERR_INTERNAL, "wimext_locate: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    *out_off = 0;
    if (want_len < head_len || want_len > image_size) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "a %llu-byte member cannot live inside a %llu-byte "
                      "image", (unsigned long long)want_len,
                      (unsigned long long)image_size);
        return SGN_ERR_FORMAT;
    }

    /* Only offsets at which the whole member would still fit are worth
     * testing, so the scan stops well before the end of a large ISO. */
    while (base <= image_size - want_len) {
        size_t got = 0;
        size_t i;

        if (rd(ctx, base, g_scan, sizeof(g_scan), &got) != 0) {
            sgn_set_error(SGN_ERR_IO, "read at %llu while scanning for the "
                          "WIM", (unsigned long long)base);
            return SGN_ERR_IO;
        }
        if (got < sizeof(k_magic))
            break;

        for (i = 0; i + sizeof(k_magic) <= got; i += WIMEXT_ALIGN) {
            uint64_t cand = base + (uint64_t)i;
            size_t hgot = 0;
            wim_header h;

            if (g_scan[i] != k_magic[0] ||
                memcmp(&g_scan[i], k_magic, sizeof(k_magic)) != 0)
                continue;
            if (cand > image_size - want_len)
                break;
            if (rd(ctx, cand, g_head, head_len, &hgot) != 0 ||
                hgot < head_len)
                continue;
            if (memcmp(g_head, want_head, head_len) != 0)
                continue;
            if (wim_header_parse(g_head, hgot, want_len, &h) != SGN_OK)
                continue;

            *out_off = cand;
            return SGN_OK;
        }

        if (got < sizeof(g_scan))
            break;
        base += sizeof(g_scan);
    }

    sgn_set_error(SGN_ERR_FORMAT,
                  "this %llu-byte WIM does not begin at any %u-byte boundary "
                  "of the image: it is not stored as a plain sector-aligned "
                  "extent, so it cannot be served through a byte-range splice",
                  (unsigned long long)want_len, (unsigned)WIMEXT_ALIGN);
    return SGN_ERR_FORMAT;
}

/* ------------------------------------------------------ the comparison */

/* 256 KB each, so the pair costs half a megabyte of BSS and the comparison
 * runs at read speed rather than syscall speed. */
#define CMP_CHUNK (256u << 10)

static uint8_t g_cmp_a[CMP_CHUNK];
static uint8_t g_cmp_b[CMP_CHUNK];

int wimext_compare(wimext_read_fn a, void *a_ctx, uint64_t a_off,
                   wimext_read_fn b, void *b_ctx, uint64_t b_off,
                   uint64_t len, uint64_t *first_diff)
{
    uint64_t done = 0;

    if (a == NULL || b == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "wimext_compare: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    if (first_diff != NULL)
        *first_diff = 0;

    while (done < len) {
        uint64_t left = len - done;
        size_t want = (left < (uint64_t)sizeof(g_cmp_a))
                          ? (size_t)left : sizeof(g_cmp_a);
        size_t got_a = 0;
        size_t got_b = 0;
        size_t i;

        if (a(a_ctx, a_off + done, g_cmp_a, want, &got_a) != 0 ||
            b(b_ctx, b_off + done, g_cmp_b, want, &got_b) != 0) {
            sgn_set_error(SGN_ERR_IO,
                          "read failed %llu bytes into the comparison",
                          (unsigned long long)done);
            return SGN_ERR_IO;
        }
        if (got_a < want || got_b < want) {
            if (first_diff != NULL)
                *first_diff = done + (uint64_t)((got_a < got_b) ? got_a
                                                                : got_b);
            sgn_set_error(SGN_ERR_FORMAT,
                          "one side ended %llu bytes into a %llu-byte range",
                          (unsigned long long)done, (unsigned long long)len);
            return SGN_ERR_FORMAT;
        }
        if (memcmp(g_cmp_a, g_cmp_b, want) != 0) {
            uint64_t at;

            for (i = 0; i < want; i++) {
                if (g_cmp_a[i] != g_cmp_b[i])
                    break;
            }
            at = done + (uint64_t)i;
            if (first_diff != NULL)
                *first_diff = at;
            sgn_set_error(SGN_ERR_FORMAT,
                          "the two ranges differ at byte %llu",
                          (unsigned long long)at);
            return SGN_ERR_FORMAT;
        }
        done += want;
    }
    return SGN_OK;
}
