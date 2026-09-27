/* Tests for falcon-appliance/src/stickid.c: which partitions the appliance
 * takes as its stick's. Every "no" here is a volume the appliance must not
 * open: another stick's, the machine's own, or its own stick's partition in
 * a role it does not use.
 */

#include "synth.h"
#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <stickid.h>

static const uint8_t g_id[16] = {
    0xc0, 0xff, 0xee, 0x00, 0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc
};

static ident_rec rec_of(ident_role role, const uint8_t *id)
{
    ident_rec r;

    memset(&r, 0, sizeof r);
    r.version = IDENT_VERSION;
    r.role = role;
    memcpy(r.install_id, id, 16);
    return r;
}

static void test_stickid_cred(void **state)
{
    ident_rec boot = rec_of(IDENT_ROLE_BOOT, g_id);
    ident_rec lib = rec_of(IDENT_ROLE_LIBRARY, g_id);
    ident_rec out;
    char text[IDENT_TEXT_MAX];
    size_t n;

    (void)state;
    n = ident_encode_text(&boot, text, sizeof text);
    assert_int_equal(stickid_from_cred(text, n, &out), IDENT_OK);
    assert_memory_equal(out.install_id, g_id, 16);
    assert_int_equal(out.role, IDENT_ROLE_BOOT);
    /* Without its newline, as a file written by hand might be. */
    assert_int_equal(stickid_from_cred(text, n - 1u, &out), IDENT_OK);

    /* A record of another role is not the loader's. */
    n = ident_encode_text(&lib, text, sizeof text);
    assert_int_equal(stickid_from_cred(text, n, &out), IDENT_ERR_BAD);
    assert_int_equal(stickid_from_cred("falcon 1 boot 00\n", 17, &out),
                     IDENT_ERR_BAD);
    assert_int_equal(stickid_from_cred(NULL, 1, &out), IDENT_ERR_ARG);
    assert_int_equal(stickid_from_cred(text, n, NULL), IDENT_ERR_ARG);
}

static void test_stickid_judge(void **state)
{
    static const uint8_t other[16] = { 7, 7, 7, 7, 7, 7, 7, 7,
                                       7, 7, 7, 7, 7, 7, 7, 7 };
    ident_rec install = rec_of(IDENT_ROLE_BOOT, g_id);
    ident_rec lib = rec_of(IDENT_ROLE_LIBRARY, g_id);
    ident_rec shelf = rec_of(IDENT_ROLE_SHELF, g_id);
    ident_rec boot = rec_of(IDENT_ROLE_BOOT, g_id);
    ident_rec their_lib = rec_of(IDENT_ROLE_LIBRARY, other);
    ident_rec their_shelf = rec_of(IDENT_ROLE_SHELF, other);

    (void)state;
    assert_int_equal(stickid_judge(&install, &lib, 0), STICKID_P2);
    assert_int_equal(stickid_judge(&install, &shelf, 1), STICKID_P3);
    /* A plaintext shelf has no key on it. */
    assert_int_equal(stickid_judge(&install, &shelf, 0), STICKID_NONE);
    /* A library record inside a LUKS header is not a library. */
    assert_int_equal(stickid_judge(&install, &lib, 1), STICKID_NONE);
    assert_int_equal(stickid_judge(&install, &boot, 0), STICKID_NONE);
    /* A second stick. */
    assert_int_equal(stickid_judge(&install, &their_lib, 0), STICKID_NONE);
    assert_int_equal(stickid_judge(&install, &their_shelf, 1), STICKID_NONE);
    assert_int_equal(stickid_judge(NULL, &lib, 0), STICKID_NONE);
    assert_int_equal(stickid_judge(&install, NULL, 0), STICKID_NONE);
}

/* An exFAT volume, with a /.falcon record for `r` or none. */
static void volume(synth_vol *sv, const ident_rec *r)
{
    uint8_t *root;
    unsigned n = 0;

    synth_init(sv, 9u, 2u, 32u);
    root = synth_cluster(sv, sv->root_cluster);
    if (r != NULL) {
        char text[IDENT_TEXT_MAX];
        size_t len = ident_encode_text(r, text, sizeof text);

        n = synth_put_set(root, ".falcon", 0,
                          (uint8_t)(SYNTH_FLAG_ALLOC | SYNTH_FLAG_NOFAT), 3u,
                          len);
        memcpy(synth_cluster(sv, 3u), text, len);
    }
    synth_put_end(root + n * SYNTH_ENT);
}

static int luks_head_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    static const uint8_t magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

    (void)ctx;
    memset(buf, 0, (size_t)len);
    if (off == 0)
        memcpy(buf, magic, len < 6u ? (size_t)len : 6u);
    return 0;
}

static int fail_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    (void)off;
    (void)len;
    (void)buf;
    return -1;
}

static void test_stickid_read(void **state)
{
    ident_rec lib = rec_of(IDENT_ROLE_LIBRARY, g_id);
    ident_rec out;
    synth_vol sv;
    int luks = -1;

    (void)state;
    volume(&sv, &lib);
    assert_int_equal(stickid_read(synth_read, &sv, &out, &luks), IDENT_OK);
    assert_int_equal(luks, 0);
    assert_int_equal(out.role, IDENT_ROLE_LIBRARY);
    assert_memory_equal(out.install_id, g_id, 16);
    synth_free(&sv);

    /* An exFAT volume nobody stamped. */
    volume(&sv, NULL);
    assert_int_equal(stickid_read(synth_read, &sv, &out, &luks),
                     IDENT_ERR_NONE);
    synth_free(&sv);

    /* A LUKS2 magic with no header behind it: LUKS, and no record. */
    assert_int_equal(stickid_read(luks_head_read, NULL, &out, &luks),
                     IDENT_ERR_NONE);
    assert_int_equal(luks, 1);

    assert_int_equal(stickid_read(fail_read, NULL, &out, &luks),
                     IDENT_ERR_NONE);
    assert_int_equal(stickid_read(NULL, NULL, &out, &luks), IDENT_ERR_ARG);
    assert_int_equal(stickid_read(synth_read, &sv, NULL, &luks),
                     IDENT_ERR_ARG);
    assert_int_equal(stickid_read(synth_read, &sv, &out, NULL),
                     IDENT_ERR_ARG);
}

int run_appliance_stickid_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_stickid_cred),
        cmocka_unit_test(test_stickid_judge),
        cmocka_unit_test(test_stickid_read),
    };

    return cmocka_run_group_tests_name("appliance stickid", tests, NULL,
                                       NULL);
}
