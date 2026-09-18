/* Tests for falcon-hook/src/hookcore.c: everything the hook decides, apart
 * from the system calls that act on it.
 *
 * The device-mapper table is the one that matters most, and it is checked
 * the way the kernel will read it: the struct dm_ioctl header, then each
 * target spec at data_start plus the running `next` offsets, its sector
 * range, and its "MAJ:MIN SECTOR" parameters.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <linux/dm-ioctl.h>

#include <hookcore.h>

static void test_hook_cmdline(void **state)
{
    static const char cmd[] =
        "BOOT_IMAGE=/vmlinuz root=live:CDLABEL=X rd.live.image "
        "rdinit=/falcon/hook falcon.init=/sbin/init quiet "
        "falcon.init=\"/usr/lib/init\"\n";
    char out[64];

    (void)state;
    /* The last occurrence wins, unquoted. */
    assert_int_equal(hook_cmdline_arg(cmd, sizeof cmd - 1u, "falcon.init",
                                      out, sizeof out), 1);
    assert_string_equal(out, "/usr/lib/init");
    assert_int_equal(hook_cmdline_arg(cmd, sizeof cmd - 1u, "root", out,
                                      sizeof out), 1);
    assert_string_equal(out, "live:CDLABEL=X");
    /* A key is a whole word before '=': "rd" is not "rd.live.image". */
    assert_int_equal(hook_cmdline_arg(cmd, sizeof cmd - 1u, "rd", out,
                                      sizeof out), 0);
    assert_int_equal(hook_cmdline_arg(cmd, sizeof cmd - 1u, "quiet", out,
                                      sizeof out), 0);
    assert_int_equal(hook_cmdline_arg(cmd, sizeof cmd - 1u, "falcon.init",
                                      out, 4), 0);
    assert_int_equal(hook_cmdline_arg(NULL, 0, "x", out, sizeof out), 0);
    assert_int_equal(hook_cmdline_arg(cmd, 5, "x", NULL, 1), 0);
    /* A NUL ends the line wherever it is. */
    {
        static const char nul[] = "a=1\0b=2";

        assert_int_equal(hook_cmdline_arg(nul, sizeof nul - 1u, "b", out,
                                          sizeof out), 0);
        assert_int_equal(hook_cmdline_arg(nul, sizeof nul - 1u, "a", out,
                                          sizeof out), 1);
    }
}

static void test_hook_efivar_and_names(void **state)
{
    static const uint8_t file[] = { 6, 0, 0, 0, 'F', 'L' };
    size_t n = 0;
    uint32_t maj = 0, min = 0;

    (void)state;
    assert_ptr_equal(hook_efivar_data(file, sizeof file, &n), file + 4);
    assert_int_equal(n, 2);
    assert_null(hook_efivar_data(file, 4, &n));
    assert_null(hook_efivar_data(NULL, 8, &n));

    assert_true(hook_disk_candidate("sda"));
    assert_true(hook_disk_candidate("nvme0n1"));
    assert_true(hook_disk_candidate("mmcblk0"));
    assert_true(hook_disk_candidate("vda"));
    assert_false(hook_disk_candidate("loop0"));
    assert_false(hook_disk_candidate("ram3"));
    assert_false(hook_disk_candidate("zram0"));
    assert_false(hook_disk_candidate("dm-0"));
    assert_false(hook_disk_candidate("md127"));
    assert_false(hook_disk_candidate("sr0"));
    assert_false(hook_disk_candidate("."));
    assert_false(hook_disk_candidate(""));
    assert_false(hook_disk_candidate(NULL));

    assert_true(hook_parse_devnum("8:2\n", 4, &maj, &min));
    assert_int_equal(maj, 8);
    assert_int_equal(min, 2);
    assert_true(hook_parse_devnum("259:65536", 9, &maj, &min));
    assert_int_equal(min, 65536);
    assert_false(hook_parse_devnum("8", 1, &maj, &min));
    assert_false(hook_parse_devnum(":2", 2, &maj, &min));
    assert_false(hook_parse_devnum("8:", 2, &maj, &min));
    assert_false(hook_parse_devnum("8:2x", 4, &maj, &min));
    assert_false(hook_parse_devnum("99999999999:1", 13, &maj, &min));
    assert_false(hook_parse_devnum(NULL, 1, &maj, &min));

    /* The kernel's encoding: minor low byte, major, minor high bits. */
    assert_int_equal(hook_devnum(8, 2), 0x802u);
    assert_int_equal(hook_devnum(10, 236), 0xAECu);
    assert_int_equal(hook_devnum(259, 0x12345), (0x45u) | (259u << 8) |
                                                (0x12300u << 12));
}

static uint8_t g_map[EXTMAP_HDR_LEN + 8u * EXTMAP_EXT_LEN];
static uint8_t g_tab[4096];

static void map3(extmap_hdr *h, uint64_t last_len)
{
    extmap_extent e[3] = {
        { 1u << 20, 4096u }, { 64u << 20, 8192u }, { 100u << 20, 0 }
    };
    size_t len = 0;

    e[2].length = last_len;
    memset(h, 0, sizeof(*h));
    h->block_size = 512;
    h->part_lba = 2048;
    h->n_extents = 3;
    h->iso_size = 4096u + 8192u + last_len;
    assert_int_equal(extmap_encode(h, e, g_map, sizeof g_map, &len), EXTMAP_OK);
}

static void test_hook_dm_table(void **state)
{
    const struct dm_ioctl *io = (const struct dm_ioctl *)(const void *)g_tab;
    static const char *const want[3] = {
        "8:2 2048", "8:2 131072", "8:2 204800"
    };
    static const uint64_t start[3] = { 0, 8, 24 };
    static const uint64_t len[3] = { 8, 16, 3 };
    extmap_hdr h;
    size_t n = 0, off;
    uint32_t i;

    (void)state;
    map3(&h, 1500);             /* a ragged last run rounds up */
    assert_int_equal(hook_dm_table(&h, g_map, 8, 2, 0, g_tab, sizeof g_tab,
                                   &n), HOOK_OK);
    assert_int_equal(n, hook_dm_table_size(3));
    assert_int_equal(io->version[0], DM_VERSION_MAJOR);
    assert_int_equal(io->data_size, n);
    assert_int_equal(io->data_start, sizeof(struct dm_ioctl));
    assert_int_equal(io->target_count, 3);
    assert_true(io->flags & DM_READONLY_FLAG);
    assert_string_equal(io->name, HOOK_DM_NAME);

    off = io->data_start;
    for (i = 0; i < 3; i++) {
        const struct dm_target_spec *t =
            (const struct dm_target_spec *)(const void *)(g_tab + off);

        assert_int_equal(off % 8u, 0);
        assert_string_equal(t->target_type, "linear");
        assert_int_equal(t->sector_start, start[i]);
        assert_int_equal(t->length, len[i]);
        assert_string_equal((const char *)(t + 1), want[i]);
        off += t->next;
    }

    /* The whole-disk fallback adds the partition's offset. */
    assert_int_equal(hook_dm_table(&h, g_map, 8, 0, 2048u * 512u, g_tab,
                                   sizeof g_tab, &n), HOOK_OK);
    {
        const struct dm_target_spec *t = (const struct dm_target_spec *)
            (const void *)(g_tab + io->data_start);

        assert_string_equal((const char *)(t + 1), "8:0 4096");
    }

    /* Refusals: an interior run not in whole sectors, a base that is not,
     * too small a buffer, and missing arguments. */
    {
        extmap_extent e[2] = { { 0, 1000 }, { 4096, 24 } };
        extmap_hdr bad;
        size_t len2 = 0;

        memset(&bad, 0, sizeof bad);
        bad.block_size = 512;
        bad.n_extents = 2;
        bad.iso_size = 1024;
        assert_int_equal(extmap_encode(&bad, e, g_map, sizeof g_map, &len2),
                         EXTMAP_OK);
        assert_int_equal(hook_dm_table(&bad, g_map, 8, 2, 0, g_tab,
                                       sizeof g_tab, &n), HOOK_ERR_RANGE);
    }
    map3(&h, 512);
    assert_int_equal(hook_dm_table(&h, g_map, 8, 2, 100, g_tab, sizeof g_tab,
                                   &n), HOOK_ERR_RANGE);
    assert_int_equal(hook_dm_table(&h, g_map, 8, 2, 0, g_tab, 100, &n),
                     HOOK_ERR_NOSPACE);
    assert_int_equal(hook_dm_table(NULL, g_map, 8, 2, 0, g_tab, sizeof g_tab,
                                   &n), HOOK_ERR_BAD_ARG);
    assert_int_equal(hook_dm_table(&h, g_map, 8, 2, ~(uint64_t)0 - 511u,
                                   g_tab, sizeof g_tab, &n), HOOK_ERR_RANGE);

    /* The header-only form. */
    assert_int_equal(hook_dm_header(g_tab, sizeof g_tab, 1), HOOK_OK);
    assert_string_equal(io->uuid, HOOK_DM_UUID);
    assert_int_equal(io->data_size, sizeof(struct dm_ioctl));
    assert_int_equal(hook_dm_header(g_tab, 10, 1), HOOK_ERR_BAD_ARG);
}

int run_hookcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_hook_cmdline),
        cmocka_unit_test(test_hook_efivar_and_names),
        cmocka_unit_test(test_hook_dm_table),
    };

    return cmocka_run_group_tests_name("falcon-hook", tests, NULL, NULL);
}
