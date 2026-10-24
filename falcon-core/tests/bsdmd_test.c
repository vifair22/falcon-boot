/* Tests for falcon-core/src/media/bsdmd.c: the two byte edits that make a BSD
 * ISO boot itself as a memory disk (DESIGN Section 6.4).
 *
 * The contracts that matter are that no existing byte moves (the record goes
 * in a directory extent's free tail, the config is exactly as long as the
 * comment block it replaces) and that a medium with no room is refused rather
 * than corrupted.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <media/bsdmd.h>

#define SEC      2048u
#define SECTORS  24u
#define ROOT_LBA 20u
#define BOOT_LBA 22u

static uint8_t g_iso[SECTORS * SEC];
static int g_fail_at = -1;       /* sector whose read fails, or -1 */

static int iso_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (g_fail_at >= 0 && off / SEC == (uint64_t)g_fail_at)
        return -1;
    if (off + len > sizeof g_iso)
        return -1;
    memcpy(buf, g_iso + off, (size_t)len);
    return 0;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A directory record: name, extent and size, with the directory flag when
 * the name is a directory's. */
static uint32_t put_rec(uint8_t *at, const char *name, size_t nlen,
                        uint32_t lba, uint32_t size, int is_dir)
{
    uint32_t rl = (uint32_t)(33u + nlen);

    if (rl % 2u)
        rl++;
    memset(at, 0, rl);
    at[0] = (uint8_t)rl;
    put_le32(at + 2, lba);
    put_le32(at + 10, size);
    at[25] = is_dir ? 0x02 : 0x00;
    at[32] = (uint8_t)nlen;
    memcpy(at + 33, name, nlen);
    return rl;
}

/* Pads a directory sector with records until exactly `leave` bytes of it
 * remain, so a test can put the free tail where it wants it. */
static uint32_t pad_to_leave(uint8_t *sec, uint32_t o, uint32_t leave)
{
    static const char name[200] = { 0 };

    while (SEC - o > leave) {
        uint32_t rl = SEC - o - leave;

        if (rl > 200u)
            rl = 200u;
        rl &= ~1u;                   /* put_rec rounds a record up to even */
        if (rl < 34u)
            break;
        o += put_rec(sec + o, name, rl - 33u, 100, 2048, 0);
    }
    return o;
}

/* A minimal but well-formed medium: a primary volume descriptor, a root
 * directory of one sector holding "." ".." and BOOT, and a BOOT directory of
 * one sector holding "." and "..". root_leave and boot_leave say how many
 * free bytes each directory's sector ends with. */
static void build_iso(uint32_t root_leave, uint32_t boot_leave)
{
    uint8_t *root = g_iso + ROOT_LBA * SEC;
    uint8_t *boot = g_iso + BOOT_LBA * SEC;
    uint8_t *pvd = g_iso + 16u * SEC;
    uint32_t o;

    memset(g_iso, 0, sizeof g_iso);
    g_fail_at = -1;

    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    (void)put_rec(pvd + 156, "\0", 1, ROOT_LBA, SEC, 1);

    o = put_rec(root, "\0", 1, ROOT_LBA, SEC, 1);
    o += put_rec(root + o, "\1", 1, ROOT_LBA, SEC, 1);
    o += put_rec(root + o, "BOOT", 4, BOOT_LBA, SEC, 1);
    (void)pad_to_leave(root, o, root_leave);

    o = put_rec(boot, "\0", 1, BOOT_LBA, SEC, 1);
    o += put_rec(boot + o, "\1", 1, ROOT_LBA, SEC, 1);
    (void)pad_to_leave(boot, o, boot_leave);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void test_bsdmd_record_root(void **state)
{
    bsdmd_record_edit e;
    char path[BSDMD_PATH_MAX];
    uint64_t iso_size = (uint64_t)SECTORS * SEC;

    (void)state;
    build_iso(SEC, SEC);
    assert_int_equal(bsdmd_record(iso_size, 0, iso_read, NULL, &e, path,
                                  sizeof path), BSDMD_OK);
    assert_string_equal(path, "/FALCONBS.IMG;1");
    assert_int_equal(e.len, 48u);               /* 33 + 14, padded to even */
    /* It lands on the first free byte of the root extent, and nothing that
     * was already there has moved. */
    assert_true(e.off > (uint64_t)ROOT_LBA * SEC);
    assert_true(e.off + e.len <= (uint64_t)(ROOT_LBA + 1u) * SEC);
    assert_int_equal(g_iso[e.off], 0);          /* was free space */

    assert_int_equal(e.bytes[0], 48);
    /* The file is the medium: extent 0, length the whole image, both
     * endiannesses. */
    assert_int_equal(le32(e.bytes + 2), 0);
    assert_int_equal(be32(e.bytes + 6), 0);
    assert_int_equal(le32(e.bytes + 10), (uint32_t)iso_size);
    assert_int_equal(be32(e.bytes + 14), (uint32_t)iso_size);
    assert_int_equal(e.bytes[25], 0);           /* a file, not a directory */
    assert_int_equal(e.bytes[32], 14);
    assert_memory_equal(e.bytes + 33, "FALCONBS.IMG;1", 14);
    assert_int_equal(e.bytes[47], 0);           /* the pad byte */

    /* A window past the medium (bsdmd.h): the extent names it. */
    assert_int_equal(bsdmd_record(iso_size, SECTORS, iso_read, NULL, &e, path,
                                  sizeof path), BSDMD_OK);
    assert_int_equal(le32(e.bytes + 2), SECTORS);
    assert_int_equal(be32(e.bytes + 6), SECTORS);
    assert_int_equal(le32(e.bytes + 10), (uint32_t)iso_size);
}

/* A root directory with no room falls through to /boot rather than failing:
 * the record may live anywhere the loader can walk to. */
static void test_bsdmd_record_boot(void **state)
{
    bsdmd_record_edit e;
    char path[BSDMD_PATH_MAX];

    (void)state;
    /* 20 free bytes is less than the 48 a record needs. */
    build_iso(20, SEC);
    assert_int_equal(bsdmd_record((uint64_t)SECTORS * SEC, 0, iso_read, NULL,
                                  &e, path, sizeof path), BSDMD_OK);
    assert_string_equal(path, "/boot/FALCONBS.IMG;1");
    assert_true(e.off >= (uint64_t)BOOT_LBA * SEC);
    assert_true(e.off + e.len <= (uint64_t)(BOOT_LBA + 1u) * SEC);
}

static void test_bsdmd_record_no_room(void **state)
{
    bsdmd_record_edit e;
    char path[BSDMD_PATH_MAX];

    (void)state;
    build_iso(20, 20);
    assert_int_equal(bsdmd_record((uint64_t)SECTORS * SEC, 0, iso_read, NULL,
                                  &e, path, sizeof path), BSDMD_ERR_NO_ROOM);
}

static void test_bsdmd_record_errors(void **state)
{
    bsdmd_record_edit e;
    char path[BSDMD_PATH_MAX];
    char small[4];

    (void)state;
    build_iso(SEC, SEC);
    assert_int_equal(bsdmd_record(0, 0, iso_read, NULL, &e, path, sizeof path),
                     BSDMD_ERR_ARG);
    assert_int_equal(bsdmd_record(1024, 0, NULL, NULL, &e, path, sizeof path),
                     BSDMD_ERR_ARG);
    assert_int_equal(bsdmd_record(1024, 0, iso_read, NULL, NULL, path,
                                  sizeof path), BSDMD_ERR_ARG);
    assert_int_equal(bsdmd_record(1024, 0, iso_read, NULL, &e, small,
                                  sizeof small), BSDMD_ERR_ARG);
    /* Past what an ISO9660 length field can express. */
    assert_int_equal(bsdmd_record(0x100000000ull, 0, iso_read, NULL, &e, path,
                                  sizeof path), BSDMD_ERR_TOO_BIG);

    /* Not a medium at all. */
    memset(g_iso, 0, sizeof g_iso);
    assert_int_equal(bsdmd_record(1024, 0, iso_read, NULL, &e, path,
                                  sizeof path), BSDMD_ERR_NOT_ISO);

    /* A read that fails is an error, never a guess. */
    build_iso(SEC, SEC);
    g_fail_at = 16;
    assert_int_equal(bsdmd_record(1024, 0, iso_read, NULL, &e, path,
                                  sizeof path), BSDMD_ERR_IO);
    g_fail_at = ROOT_LBA;
    assert_int_equal(bsdmd_record(1024, 0, iso_read, NULL, &e, path,
                                  sizeof path), BSDMD_ERR_IO);
    g_fail_at = -1;
}

/* ---- loader.conf ---- */

static const char g_head[] =
    "# This is loader.conf - a file full of useful variables that you can\n"
    "# set to change the default load behavior of your system.\n"
    "#\n"
    "\n"
    "###  Basic configuration options  ####\n"
    "exec=\"echo Loading /boot/defaults/loader.conf\"\n"
    "kernel=\"kernel\"\n";

static void test_bsdmd_config(void **state)
{
    static const char want[] =
        "md_image_load=\"YES\"\n"
        "md_image_type=\"md_image\"\n"
        "md_image_name=\"/FALCONBS.IMG;1\"\n"
        "vfs.root.mountfrom=\"cd9660:/dev/md0\"\n";
    char out[BSDMD_CFG_MAX];
    uint32_t len = 0;
    size_t block;

    (void)state;
    /* Every line up to, but not including, the first that is neither blank
     * nor a comment. */
    block = (size_t)(strstr(g_head, "exec=") - g_head);
    assert_int_equal(bsdmd_config("/FALCONBS.IMG;1", NULL, g_head,
                                  strlen(g_head), out, sizeof out, &len),
                     BSDMD_OK);
    assert_int_equal(len, block);
    assert_memory_equal(out, want, strlen(want));
    /* The remainder is one comment, so the file's length never changes. */
    assert_int_equal(out[strlen(want)], '#');
    assert_int_equal(out[len - 1u], '\n');
    assert_int_equal(out[len - 2u], ' ');
}

/* The console setting falcon-bios asks for and falcon-efi does not: a BIOS
 * machine publishes no console of its own, so without it FreeBSD's installer
 * draws to the screen alone and the serial log an automated run reads stays
 * empty. */
static void test_bsdmd_config_console(void **state)
{
    static const char want[] =
        "md_image_load=\"YES\"\n"
        "md_image_type=\"md_image\"\n"
        "md_image_name=\"/FALCONBS.IMG;1\"\n"
        "vfs.root.mountfrom=\"cd9660:/dev/md0\"\n"
        "console=\"comconsole,vidconsole\"\n";
    char out[BSDMD_CFG_MAX];
    uint32_t len = 0;
    size_t block;

    (void)state;
    block = (size_t)(strstr(g_head, "exec=") - g_head);
    assert_int_equal(bsdmd_config("/FALCONBS.IMG;1", "comconsole,vidconsole",
                                  g_head, strlen(g_head), out, sizeof out,
                                  &len), BSDMD_OK);
    /* Still exactly the block it overwrote: the file's length never changes,
     * whatever is written into it. */
    assert_int_equal(len, block);
    assert_memory_equal(out, want, strlen(want));
    assert_int_equal(out[strlen(want)], '#');
    assert_int_equal(out[len - 1u], '\n');

    /* A block with room for the settings but not for the console line is
     * refused rather than truncated: a half-written setting would be a
     * loader.conf FreeBSD parses into something nobody meant. */
    {
        char head[160];
        size_t n = strlen(want) - strlen("console=\"comconsole,vidconsole\"\n")
                   + 8u;
        size_t i;

        for (i = 0; i + 1u < n && i + 1u < sizeof head; i++)
            head[i] = '#';
        head[i++] = '\n';
        head[i] = '\0';
        assert_int_equal(bsdmd_config("/FALCONBS.IMG;1",
                                      "comconsole,vidconsole", head, i, out,
                                      sizeof out, &len), BSDMD_ERR_NO_ROOM);
    }
}

static void test_bsdmd_config_errors(void **state)
{
    char out[BSDMD_CFG_MAX];
    uint32_t len = 0;

    (void)state;
    assert_int_equal(bsdmd_config(NULL, NULL, g_head, strlen(g_head), out,
                                  sizeof out, &len), BSDMD_ERR_ARG);
    assert_int_equal(bsdmd_config("/x;1", NULL, NULL, 10, out, sizeof out,
                                  &len), BSDMD_ERR_ARG);
    assert_int_equal(bsdmd_config("/x;1", NULL, g_head, strlen(g_head), NULL,
                                  sizeof out, &len), BSDMD_ERR_ARG);
    assert_int_equal(bsdmd_config("/x;1", NULL, g_head, strlen(g_head), out,
                                  sizeof out, NULL), BSDMD_ERR_ARG);
    /* A file that opens with a setting has nothing that may be overwritten. */
    assert_int_equal(bsdmd_config("/x;1", NULL, "kernel=\"kernel\"\n", 16,
                                  out, sizeof out, &len), BSDMD_ERR_NO_ROOM);
    /* A comment block too short to hold the settings. */
    assert_int_equal(bsdmd_config("/FALCONBS.IMG;1", NULL, "# short\nx=1\n",
                                  12, out, sizeof out, &len),
                     BSDMD_ERR_NO_ROOM);
}

int run_bsdmd_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bsdmd_record_root),
        cmocka_unit_test(test_bsdmd_record_boot),
        cmocka_unit_test(test_bsdmd_record_no_room),
        cmocka_unit_test(test_bsdmd_record_errors),
        cmocka_unit_test(test_bsdmd_config),
        cmocka_unit_test(test_bsdmd_config_console),
        cmocka_unit_test(test_bsdmd_config_errors),
    };

    return cmocka_run_group_tests_name("bsdmd", tests, NULL, NULL);
}
