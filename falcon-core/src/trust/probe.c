#include <trust/probe.h>

#include <lib/string.h>

/* A first read that covers the headers of every image in practice
 * (SizeOfHeaders is 0x400 for mingw and MSVC, 0x1000 at the outside), and a
 * second, larger one for the image whose section table is further in. */
#define HEAD_FIRST  4096u
#define HEAD_MAX    65536u
#define SBAT_MAX    4096u
#define SCRATCH_LEN 65536u

static uint8_t g_head[HEAD_MAX];
static uint8_t g_certs[AC_CERTS_MAX];
static uint8_t g_sbat[SBAT_MAX];
static uint8_t g_scratch[SCRATCH_LEN];
static uint8_t g_hash[32];
static ac_layout g_layout;

static size_t window(uint64_t size, size_t cap)
{
    return size < cap ? (size_t)size : cap;
}

void trust_probe(const trust_stores *st, ac_read_fn read, void *ctx,
                 uint64_t size, trust_result *out)
{
    trust_image im;
    size_t n;
    int rc;

    if (out == (trust_result *)0)
        return;
    memset(out, 0, sizeof(*out));
    if (st == (const trust_stores *)0 || read == (ac_read_fn)0 || size == 0)
        return;

    n = window(size, HEAD_FIRST);
    if (read(ctx, 0, n, g_head) != 0)
        return;
    rc = ac_layout_read(g_head, n, size, &g_layout);
    if (rc == AC_ERR_SHORT && size > n) {
        n = window(size, HEAD_MAX);
        if (read(ctx, 0, n, g_head) != 0)
            return;
        rc = ac_layout_read(g_head, n, size, &g_layout);
    }
    if (rc != AC_OK || g_layout.certs_len > AC_CERTS_MAX)
        return;

    memset(&im, 0, sizeof(im));
    if (g_layout.certs_len != 0) {
        if (read(ctx, g_layout.certs_off, g_layout.certs_len, g_certs) != 0)
            return;
        im.certs.p = g_certs;
        im.certs.len = g_layout.certs_len;
    }
    /* An SBAT section too large to be real is one this code does not read;
     * that costs a possible revocation, never a false one. */
    if (g_layout.sbat_len != 0 && g_layout.sbat_len <= SBAT_MAX) {
        if (read(ctx, g_layout.sbat_off, g_layout.sbat_len, g_sbat) != 0)
            return;
        im.sbat.p = g_sbat;
        im.sbat.len = g_layout.sbat_len;
    }
    if (size <= TRUST_HASH_MAX &&
        ac_hash(&g_layout, read, ctx, g_scratch, sizeof(g_scratch),
                g_hash) == AC_OK)
        im.hash = g_hash;

    trust_evaluate(st, &im, out);
}
