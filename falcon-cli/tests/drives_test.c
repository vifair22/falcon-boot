/* Tests for falcon-cli/src/mkstick/drives.c: the drives interactive
 * `create` offers, and the ones it lists but will not write.
 *
 * One tree under a temporary directory stands in for /sys, /proc/mounts and
 * /proc/swaps: a USB stick, a fixed disk carrying the system through LUKS
 * and LVM, disks with a partition mounted, swapped on or held by a mapping,
 * an idle fixed disk, an empty card-reader slot and a loop device.
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
#include <sys/stat.h>
#include <unistd.h>

#include <mkstick/drives.h>

static char g_root[64];
static char g_sys[128];
static char g_mounts[128];
static char g_swaps[128];

static void mkdirs(const char *rel)
{
    char cmd[512];

    (void)snprintf(cmd, sizeof cmd, "mkdir -p '%s/%s'", g_sys, rel);
    assert_int_equal(system(cmd), 0);
}

static void put(const char *rel, const char *text)
{
    char path[512];
    FILE *f;

    (void)snprintf(path, sizeof path, "%s/%s", g_sys, rel);
    f = fopen(path, "w");
    assert_non_null(f);
    assert_true(fputs(text, f) >= 0);
    assert_int_equal(fclose(f), 0);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    assert_non_null(f);
    assert_true(fputs(text, f) >= 0);
    assert_int_equal(fclose(f), 0);
}

/* A whole disk at block/NAME, its size in 512-byte sectors. */
static void disk(const char *name, const char *sectors, const char *removable)
{
    char rel[128];

    (void)snprintf(rel, sizeof rel, "block/%s/device", name);
    mkdirs(rel);
    (void)snprintf(rel, sizeof rel, "block/%s/size", name);
    put(rel, sectors);
    (void)snprintf(rel, sizeof rel, "block/%s/removable", name);
    put(rel, removable);
}

static void part(const char *disk_, const char *name, const char *sectors)
{
    char rel[128];

    (void)snprintf(rel, sizeof rel, "block/%s/%s/holders", disk_, name);
    mkdirs(rel);
    (void)snprintf(rel, sizeof rel, "block/%s/%s/partition", disk_, name);
    put(rel, "1\n");
    (void)snprintf(rel, sizeof rel, "block/%s/%s/size", disk_, name);
    put(rel, sectors);
}

static void holder(const char *under, const char *dm)
{
    char rel[128];

    (void)snprintf(rel, sizeof rel, "block/%s/holders/%s", under, dm);
    mkdirs(rel);
    (void)snprintf(rel, sizeof rel, "block/%s/holders", dm);
    mkdirs(rel);
}

static int setup(void **state)
{
    char cmd[512];

    (void)state;
    (void)snprintf(g_root, sizeof g_root, "/tmp/falcon-drives-test.XXXXXX");
    if (mkdtemp(g_root) == NULL)
        return -1;
    (void)snprintf(g_sys, sizeof g_sys, "%s/sys", g_root);
    (void)snprintf(g_mounts, sizeof g_mounts, "%s/mounts", g_root);
    (void)snprintf(g_swaps, sizeof g_swaps, "%s/swaps", g_root);

    /* The USB stick: block/sdb links through the USB host controller. */
    (void)snprintf(cmd, sizeof cmd,
                   "mkdir -p '%s/devices/pci0000:00/usb1/1-1/block/sdb' "
                   "'%s/block' && ln -s "
                   "'../devices/pci0000:00/usb1/1-1/block/sdb' '%s/block/sdb'",
                   g_sys, g_sys, g_sys);
    if (system(cmd) != 0)
        return -1;
    disk("sdb", "31260672\n", "0\n");
    put("block/sdb/device/vendor", "SanDisk \n");
    put("block/sdb/device/model", "Ultra           \n");
    part("sdb", "sdb10", "2048\n");
    part("sdb", "sdb2", "4096\n");
    part("sdb", "sdb1", "131072\n");

    /* The system: root on LVM on LUKS on nvme0n1p2. */
    disk("nvme0n1", "3907029168\n", "0\n");
    put("block/nvme0n1/device/model", "Samsung SSD 990 PRO 2TB\n");
    part("nvme0n1", "nvme0n1p1", "2097152\n");
    part("nvme0n1", "nvme0n1p2", "3904929792\n");
    holder("nvme0n1/nvme0n1p2", "dm-0");
    holder("dm-0", "dm-1");

    disk("sdc", "1000000\n", "0\n");
    part("sdc", "sdc1", "999000\n");
    disk("sdd", "1000000\n", "0\n");
    part("sdd", "sdd1", "999000\n");
    disk("sde", "1000000\n", "0\n");
    part("sde", "sde1", "999000\n");
    holder("sde/sde1", "dm-5");
    /* An idle fixed disk, an SD reader with no card, a loop device. */
    disk("sda", "2000000\n", "0\n");
    disk("sdf", "0\n", "1\n");
    disk("loop0", "2000\n", "0\n");
    /* A card reader's card: removable, not USB. */
    disk("mmcblk0", "60000000\n", "1\n");

    write_file(g_mounts, "proc /proc proc rw 0 0\n"
                         "/dev/dm-1 / xfs rw 0 0\n"
                         "/dev/sdc1 /data ext4 rw 0 0\n");
    write_file(g_swaps, "Filename Type Size Used Priority\n"
                        "/dev/sdd1 partition 499500 0 -2\n");
    return 0;
}

static int teardown(void **state)
{
    char cmd[128];

    (void)state;
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    return system(cmd) == 0 ? 0 : -1;
}

static void list(drv_entry *out, size_t *n)
{
    drv_env env;

    env.sys = g_sys;
    env.dev = "/dev";
    env.mounts = g_mounts;
    env.swaps = g_swaps;
    assert_int_equal(drv_list(&env, out, DRV_MAX, n), CLI_OK);
}

static void test_order_and_details(void **state)
{
    static drv_entry d[DRV_MAX];
    size_t n = 0;

    (void)state;
    list(d, &n);
    /* loop0 and the empty slot are left out. */
    assert_int_equal(n, 7);
    assert_string_equal(d[0].name, "mmcblk0");
    assert_string_equal(d[1].name, "sdb");
    assert_string_equal(d[2].name, "nvme0n1");
    assert_string_equal(d[3].name, "sda");
    assert_string_equal(d[6].name, "sde");

    assert_true(d[1].usb);
    assert_false(d[1].removable);
    assert_true(d[0].removable);
    assert_false(d[0].usb);
    assert_string_equal(d[1].path, "/dev/sdb");
    assert_string_equal(d[1].model, "SanDisk Ultra");
    assert_true(d[1].size == 31260672ull * 512u);
    assert_string_equal(d[1].busy, "");

    /* Partitions in the kernel's order. */
    assert_int_equal(d[1].n_parts, 3);
    assert_string_equal(d[1].part[0], "sdb1");
    assert_string_equal(d[1].part[1], "sdb2");
    assert_string_equal(d[1].part[2], "sdb10");
    assert_true(d[1].part_size[0] == 131072ull * 512u);
    assert_true(d[1].part_size[2] == 2048ull * 512u);
}

static void test_in_use(void **state)
{
    static drv_entry d[DRV_MAX];
    size_t n = 0;

    (void)state;
    list(d, &n);
    /* Root through two mappings is the system disk. */
    assert_true(d[2].system);
    assert_string_equal(d[2].busy, "this machine's system disk");
    assert_string_equal(d[2].model, "Samsung SSD 990 PRO 2TB");
    assert_string_equal(d[3].busy, "");
    assert_false(d[3].system);
    assert_string_equal(d[4].busy, "mounted at /data");
    assert_string_equal(d[5].busy, "used as swap");
    assert_string_equal(d[6].busy, "held by dm-5");
}

static void test_no_sysfs(void **state)
{
    static drv_entry d[DRV_MAX];
    drv_env env;
    size_t n = 7;

    (void)state;
    drv_env_default(&env);
    env.sys = "/nonexistent-falcon-sys";
    assert_int_equal(drv_list(&env, d, DRV_MAX, &n), CLI_ERR_IO);
    assert_int_equal(n, 0);
}

int run_drives_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_order_and_details),
        cmocka_unit_test(test_in_use),
        cmocka_unit_test(test_no_sysfs),
    };

    return cmocka_run_group_tests_name("mkstick drives", tests, setup,
                                       teardown);
}
