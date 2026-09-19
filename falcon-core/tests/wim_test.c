/* Tests for falcon-core/src/wim: the LZX decompressor and the WIM reader,
 * against a WIM wimlib wrote (falcon-core/tools/mk-test-wim.sh), and the
 * winpeshl patch, against wimlib reading the result back.
 *
 * The decompressor's oracle is the WIM itself: its lookup table names every
 * stream by the SHA-1 of its uncompressed bytes, so a resource decompressed
 * right hashes to its own name. Every resource in the fixture is read that
 * way. FALCON_TEST_BOOTWIM names a real boot.wim to put through the same
 * check (and the patch) when set.
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
#include <sys/stat.h>
#include <unistd.h>

#include <crypto/sha1.h>
#include <wim/lzx.h>
#include <wim/wim.h>

#ifndef MEDIA_FIXTURE_DIR
#define MEDIA_FIXTURE_DIR "build"
#endif

static int g_fd = -1;
static const char g_cfg[] = "iso=Windows/Win11.iso\nsize=7736125440\n";
static uint8_t g_scratch[WIM_SCRATCH_LEN];

static int rd(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    return pread(g_fd, buf, (size_t)len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

static uint64_t open_wim(const char *path)
{
    struct stat st;

    if (g_fd >= 0)
        close(g_fd);
    g_fd = open(path, O_RDONLY);
    assert_true(g_fd >= 0);
    assert_int_equal(fstat(g_fd, &st), 0);
    return (uint64_t)st.st_size;
}

static void fixture(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", MEDIA_FIXTURE_DIR, name);
}

/* Every resource read and hashed; returns how many were compressed. */
static size_t check_all(const wim_info *w)
{
    uint8_t *table = malloc(w->lookup.orig);
    size_t n = w->lookup.orig / WIM_LOOKUP_LEN, i, compressed = 0;

    assert_non_null(table);
    assert_int_equal(rd(NULL, w->lookup.offset, w->lookup.orig, table), 0);
    for (i = 0; i < n; i++) {
        wim_reshdr r;
        uint8_t hash[WIM_HASH_LEN], got[WIM_HASH_LEN];
        uint8_t *buf;

        wim_lookup_entry(table, i, &r, NULL, hash);
        buf = malloc(r.orig ? r.orig : 1);
        assert_non_null(buf);
        assert_int_equal(wim_read_resource(w, rd, NULL, &r, buf, r.orig,
                                           g_scratch), WIM_OK);
        sha1(buf, r.orig, got);
        if (memcmp(got, hash, WIM_HASH_LEN) != 0)
            fail_msg("resource %zu (flags 0x%x, %llu bytes) does not hash to "
                     "its name", i, r.flags, (unsigned long long)r.orig);
        compressed += (r.flags & WIM_RES_COMPRESSED) != 0;
        free(buf);
    }
    free(table);
    return compressed;
}

static void test_wim_reads_fixture(void **state)
{
    char path[512];
    wim_info w;
    uint64_t size;

    (void)state;
    fixture(path, sizeof path, "test-wim.wim");
    size = open_wim(path);
    assert_int_equal(wim_open(rd, NULL, size, &w), WIM_OK);
    assert_int_equal(w.image_count, 2);
    assert_int_equal(w.boot_index, 2);
    assert_true(check_all(&w) > 0);
}

static int have_wimlib(void)
{
    return system("command -v wimlib-imagex >/dev/null 2>&1") == 0;
}

/* Patches the WIM at in (size bytes) with jump, writes the served result to
 * out, and returns 0. */
static void patch_to(const char *in, const char *out, const uint8_t *jump,
                     size_t jump_len)
{
    wim_info w;
    uint64_t size = open_wim(in);
    uint8_t *lookup, *meta, *tail, hdr[WIM_HDR_LEN];
    size_t cap, tail_len = 0;
    FILE *f;
    uint8_t buf[65536];
    uint64_t off;

    assert_int_equal(wim_open(rd, NULL, size, &w), WIM_OK);
    lookup = malloc(w.lookup.orig);
    meta = malloc(w.boot_meta.orig);
    assert_non_null(lookup);
    assert_non_null(meta);
    assert_int_equal(rd(NULL, w.lookup.offset, w.lookup.orig, lookup), 0);
    assert_int_equal(wim_read_resource(&w, rd, NULL, &w.boot_meta, meta,
                                       w.boot_meta.orig, g_scratch), WIM_OK);
    cap = wim_patch_tail_max(w.boot_meta.orig, w.lookup.orig, jump_len,
                             sizeof g_cfg - 1u);
    tail = malloc(cap);
    assert_non_null(tail);
    assert_int_equal(wim_patch_build(&w, lookup, meta, w.boot_meta.orig, jump,
                                     jump_len, g_cfg, sizeof g_cfg - 1u, hdr,
                                     tail, cap, &tail_len), WIM_OK);
    assert_true(tail_len <= cap);

    f = fopen(out, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(hdr, 1, sizeof hdr, f), sizeof hdr);
    for (off = WIM_HDR_LEN; off < size;) {
        size_t n = size - off > sizeof buf ? sizeof buf : (size_t)(size - off);

        assert_int_equal(rd(NULL, off, n, buf), 0);
        assert_int_equal(fwrite(buf, 1, n, f), n);
        off += n;
    }
    assert_int_equal(fwrite(tail, 1, tail_len, f), tail_len);
    assert_int_equal(fclose(f), 0);
    free(tail);
    free(meta);
    free(lookup);
}

static void check_patched(const char *patched, const char *orig_pe,
                          const char *jump_path)
{
    char cmd[4096];
    wim_info w;
    uint64_t size;

    /* Our reader: every resource, including the new ones, hashes right. */
    size = open_wim(patched);
    assert_int_equal(wim_open(rd, NULL, size, &w), WIM_OK);
    (void)check_all(&w);

    if (!have_wimlib()) {
        print_message("  (wimlib-imagex not found; not checked with wimlib)\n");
        return;
    }
    snprintf(cmd, sizeof cmd,
             "set -e; d=$(mktemp -d); trap 'rm -rf \"$d\"' EXIT; "
             "wimlib-imagex verify '%s' >/dev/null; "
             "wimlib-imagex extract '%s' %u /Windows/System32/winpeshl.exe "
             "/Windows/System32/winpeshl_orig.exe "
             "/Windows/System32/falcon-winjump.cfg --dest-dir=\"$d\" "
             "--no-acls >/dev/null; "
             "cmp \"$d/winpeshl.exe\" '%s'; "
             "printf 'iso=Windows/Win11.iso\\nsize=7736125440\\n' | "
             "cmp - \"$d/falcon-winjump.cfg\"; "
             "%s",
             patched, patched, w.boot_index, jump_path, orig_pe);
    assert_int_equal(system(cmd), 0);
}

static void test_wim_patch_fixture(void **state)
{
    char in[512], out[512], jump_path[512], cmd[2048];
    uint8_t jump[256];
    FILE *f;
    size_t jump_len;

    (void)state;
    fixture(in, sizeof in, "test-wim.wim");
    fixture(out, sizeof out, "test-wim-patched.wim");
    fixture(jump_path, sizeof jump_path, "test-wim.jump");
    f = fopen(jump_path, "rb");
    assert_non_null(f);
    jump_len = fread(jump, 1, sizeof jump, f);
    fclose(f);
    assert_true(jump_len > 0);
    patch_to(in, out, jump, jump_len);
    snprintf(cmd, sizeof cmd,
             "printf 'the original winpeshl.exe\\n' | "
             "cmp - \"$d/winpeshl_orig.exe\"");
    check_patched(out, cmd, jump_path);
}

/* A real boot.wim, when the environment names one. */
static void test_wim_real_bootwim(void **state)
{
    const char *path = getenv("FALCON_TEST_BOOTWIM");
    char out[512], jump_path[512], cmd[1024];
    static const uint8_t jump[] = "MZ stand-in for falcon-winjump";
    wim_info w;
    FILE *f;

    (void)state;
    if (path == NULL || path[0] == '\0') {
        skip();
        return;
    }
    assert_int_equal(wim_open(rd, NULL, open_wim(path), &w), WIM_OK);
    (void)check_all(&w);
    fixture(out, sizeof out, "test-bootwim-patched.wim");
    fixture(jump_path, sizeof jump_path, "test-bootwim.jump");
    f = fopen(jump_path, "wb");
    assert_non_null(f);
    fwrite(jump, 1, sizeof jump - 1u, f);
    fclose(f);
    patch_to(path, out, jump, sizeof jump - 1u);
    snprintf(cmd, sizeof cmd,
             "wimlib-imagex extract '%s' 2 /Windows/System32/winpeshl.exe "
             "--dest-dir=\"$d/o\" --no-acls >/dev/null && "
             "cmp \"$d/o/winpeshl.exe\" \"$d/winpeshl_orig.exe\"", path);
    check_patched(out, cmd, jump_path);
    unlink(out);
}

static void test_wim_refusals(void **state)
{
    static const uint8_t junk[16] = { 0 };
    uint8_t out[64];
    wim_info w;
    char path[512];

    (void)state;
    assert_int_equal(lzx_decompress(NULL, 1, out, 1), LZX_ERR_BAD_ARG);
    assert_int_equal(lzx_decompress(junk, sizeof junk, out, 0),
                     LZX_ERR_BAD_ARG);
    assert_int_equal(lzx_decompress(junk, sizeof junk, out, LZX_CHUNK_MAX + 1),
                     LZX_ERR_BAD_ARG);
    /* Block type 0 is not one. */
    assert_int_equal(lzx_decompress(junk, sizeof junk, out, sizeof out),
                     LZX_ERR_CORRUPT);

    fixture(path, sizeof path, "test-wim.jump");
    assert_int_equal(wim_open(rd, NULL, open_wim(path), &w), WIM_ERR_FORMAT);
    assert_int_equal(wim_open(NULL, NULL, 0, &w), WIM_ERR_BAD_ARG);
}

static int teardown(void **state)
{
    (void)state;
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
    return 0;
}

int run_wim_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_wim_reads_fixture, teardown),
        cmocka_unit_test_teardown(test_wim_patch_fixture, teardown),
        cmocka_unit_test_teardown(test_wim_real_bootwim, teardown),
        cmocka_unit_test_teardown(test_wim_refusals, teardown),
    };

    return cmocka_run_group_tests_name("wim", tests, NULL, NULL);
}
