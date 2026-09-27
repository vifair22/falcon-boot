/* Tests for falcon-core/src/ident/identfs.c: a volume's /.falcon record,
 * read through the fs registry.
 *
 * The volumes are synthetic exFAT from the synth builder, with the record
 * (or something that is not one) written into the root. Every answer but
 * IDENT_OK means "not identified", so each refusal is a volume a loader
 * must not take.
 */

#include "synth.h"
#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <ident/identfs.h>

#define REC_CLUSTER 3u

static const uint8_t g_id[16] = {
    0x5a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x0f
};

/* The volume with a root entry named `name` holding `text`, or no entry at
 * all when name is NULL. dir makes the entry a directory instead. */
static void volume(synth_vol *sv, const char *name, const char *text, int dir)
{
    uint8_t *root;
    unsigned n = 0;
    size_t len = text != NULL ? strlen(text) : 0u;

    synth_init(sv, 9u, 2u, 32u);
    root = synth_cluster(sv, sv->root_cluster);
    if (name != NULL) {
        n = synth_put_set(root, name, dir ? SYNTH_ATTR_DIR : 0u,
                          (uint8_t)(SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT),
                          REC_CLUSTER,
                          dir ? synth_cluster_bytes(sv) : (uint64_t)len);
        if (dir)
            synth_put_end(synth_cluster(sv, REC_CLUSTER));
        else
            memcpy(synth_cluster(sv, REC_CLUSTER), text, len);
    }
    synth_put_end(root + n * SYNTH_ENT);
}

static void record_text(ident_role role, const uint8_t *id, char *out)
{
    ident_rec r;

    memset(&r, 0, sizeof r);
    r.version = IDENT_VERSION;
    r.role = role;
    memcpy(r.install_id, id, 16);
    assert_true(ident_encode_text(&r, out, IDENT_TEXT_MAX) > 0u);
}

/* Zeros: no filesystem at all. */
static int zero_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    (void)off;
    memset(buf, 0, (size_t)len);
    return 0;
}

static ident_fs_scratch g_s;

static void test_identfs_reads_the_record(void **state)
{
    synth_vol sv;
    char text[IDENT_TEXT_MAX];
    ident_rec r;

    (void)state;
    record_text(IDENT_ROLE_LIBRARY, g_id, text);
    volume(&sv, ".falcon", text, 0);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_OK);
    assert_int_equal(r.role, IDENT_ROLE_LIBRARY);
    assert_memory_equal(r.install_id, g_id, 16);
    /* The volume stays mounted, and reads the same again. */
    memset(&r, 0, sizeof r);
    assert_int_equal(ident_read_vol(&g_s.vol, &g_s, &r), IDENT_OK);
    assert_memory_equal(r.install_id, g_id, 16);
    synth_free(&sv);

    /* The name is matched as fs_lookup matches: case does not matter. */
    volume(&sv, ".FALCON", text, 0);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_OK);
    synth_free(&sv);
}

static void test_identfs_refusals(void **state)
{
    synth_vol sv;
    char text[IDENT_TEXT_MAX];
    char big[IDENT_TEXT_MAX + 8u];
    ident_rec r;

    (void)state;
    /* No file. */
    volume(&sv, NULL, NULL, 0);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_ERR_NONE);
    synth_free(&sv);
    /* A file by another name. */
    record_text(IDENT_ROLE_LIBRARY, g_id, text);
    volume(&sv, "falcon", text, 0);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_ERR_NONE);
    synth_free(&sv);
    /* A directory by the name. */
    volume(&sv, ".falcon", NULL, 1);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_ERR_NONE);
    synth_free(&sv);
    /* Empty. */
    volume(&sv, ".falcon", "", 0);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_ERR_BAD);
    synth_free(&sv);
    /* Too long to be one record, whatever it starts with. */
    memset(big, 'x', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';
    memcpy(big, text, strlen(text));
    volume(&sv, ".falcon", big, 0);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_ERR_BAD);
    synth_free(&sv);
    /* Not a record. */
    volume(&sv, ".falcon", "falcon 1 library nothex\n", 0);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, &r), IDENT_ERR_BAD);
    synth_free(&sv);
    /* Not a filesystem at all. */
    assert_int_equal(ident_probe(zero_read, NULL, &g_s, &r), IDENT_ERR_NONE);
    volume(&sv, NULL, NULL, 0);

    assert_int_equal(ident_probe(NULL, NULL, &g_s, &r), IDENT_ERR_ARG);
    assert_int_equal(ident_probe(synth_read, &sv, NULL, &r), IDENT_ERR_ARG);
    assert_int_equal(ident_probe(synth_read, &sv, &g_s, NULL), IDENT_ERR_ARG);
    g_s.vol.ops = NULL;
    assert_int_equal(ident_read_vol(&g_s.vol, &g_s, &r), IDENT_ERR_ARG);
    assert_int_equal(ident_read_vol(NULL, &g_s, &r), IDENT_ERR_ARG);
    synth_free(&sv);
}

int run_identfs_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_identfs_reads_the_record),
        cmocka_unit_test(test_identfs_refusals),
    };

    return cmocka_run_group_tests_name("identfs", tests, NULL, NULL);
}
