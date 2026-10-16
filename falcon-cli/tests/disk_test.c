/* Tests for falcon-cli/src/mkstick/disk.c's disk_luks_holder: whether a
 * shelf's OS has it open, read from sysfs (docs/DESIGN.md Section 12.14).
 *
 * Each case builds the part of /sys/class/block the lookup reads in a
 * temporary directory: the partition's holders/ and each holder's dm/uuid
 * and dm/name. Only one LUKS mapping over the partition counts; anything
 * else must send update back to the passphrase.
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

#include <mkstick/disk.h>

static char g_sys[64];

/* A directory under the tree: rel, or rel/sub when sub is given. */
static void mk(const char *rel, const char *sub)
{
    char path[512];

    if (sub != NULL)
        (void)snprintf(path, sizeof path, "%s/%s/%s", g_sys, rel, sub);
    else
        (void)snprintf(path, sizeof path, "%s/%s", g_sys, rel);
    assert_int_equal(mkdir(path, 0700), 0);
}

static void put(const char *dm, const char *file, const char *text)
{
    char path[512];
    FILE *f;

    (void)snprintf(path, sizeof path, "%s/%s/dm/%s", g_sys, dm, file);
    f = fopen(path, "w");
    assert_non_null(f);
    assert_true(fputs(text, f) >= 0);
    assert_int_equal(fclose(f), 0);
}

/* A partition; a dm device with the uuid and name given; a holder. */
static void part(const char *name)
{
    mk(name, NULL);
    mk(name, "holders");
}

static void dm(const char *dev, const char *uuid, const char *name)
{
    mk(dev, NULL);
    mk(dev, "dm");
    put(dev, "uuid", uuid);
    put(dev, "name", name);
}

static void holds(const char *partname, const char *dev)
{
    char rel[128];

    (void)snprintf(rel, sizeof rel, "%s/holders", partname);
    mk(rel, dev);
}

static int setup(void **state)
{
    (void)state;
    (void)snprintf(g_sys, sizeof g_sys, "/tmp/falcon-disk-test.XXXXXX");
    return mkdtemp(g_sys) == NULL ? -1 : 0;
}

static int teardown(void **state)
{
    char cmd[128];

    (void)state;
    (void)snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_sys);
    return system(cmd) == 0 ? 0 : -1;
}

/* The shelf as crypttab opens it: one LUKS2 mapping, named. */
static void test_open_shelf(void **state)
{
    char dev[32];
    char name[64];

    (void)state;
    part("nvme0n1p3");
    dm("dm-1", "CRYPT-LUKS2-0123456789abcdef0123456789abcdef-dtshelf\n",
       "dtshelf\n");
    holds("nvme0n1p3", "dm-1");
    assert_int_equal(disk_luks_holder(g_sys, "nvme0n1p3", dev, sizeof dev,
                                      name, sizeof name), 1);
    assert_string_equal(dev, "dm-1");
    assert_string_equal(name, "dtshelf");
}

/* A closed shelf has no holder; a partition sysfs does not know is the
 * same. */
static void test_closed(void **state)
{
    char dev[32];
    char name[64];

    (void)state;
    part("sdb3");
    assert_int_equal(disk_luks_holder(g_sys, "sdb3", dev, sizeof dev, name,
                                      sizeof name), 0);
    assert_int_equal(disk_luks_holder(g_sys, "sdz9", dev, sizeof dev, name,
                                      sizeof name), 0);
}

/* A holder that is not LUKS (LVM over the partition, plain dm-crypt) is
 * not the shelf's plaintext, and neither is one of two. */
static void test_not_luks_or_two(void **state)
{
    char dev[32];
    char name[64];

    (void)state;
    part("sdc3");
    dm("dm-4", "LVM-abcdef\n", "vg-lv\n");
    holds("sdc3", "dm-4");
    assert_int_equal(disk_luks_holder(g_sys, "sdc3", dev, sizeof dev, name,
                                      sizeof name), 0);

    part("sdd3");
    dm("dm-5", "CRYPT-PLAIN-x\n", "plain\n");
    holds("sdd3", "dm-5");
    assert_int_equal(disk_luks_holder(g_sys, "sdd3", dev, sizeof dev, name,
                                      sizeof name), 0);

    part("sde3");
    dm("dm-6", "CRYPT-LUKS2-a-one\n", "one\n");
    dm("dm-7", "CRYPT-LUKS2-b-two\n", "two\n");
    holds("sde3", "dm-6");
    holds("sde3", "dm-7");
    assert_int_equal(disk_luks_holder(g_sys, "sde3", dev, sizeof dev, name,
                                      sizeof name), 0);
}

/* A holder whose dm/ does not read, and a device name that does not fit. */
static void test_unreadable(void **state)
{
    char dev[32];
    char name[4];

    (void)state;
    part("sdf3");
    mk("dm-8", NULL);
    holds("sdf3", "dm-8");
    assert_int_equal(disk_luks_holder(g_sys, "sdf3", dev, sizeof dev, name,
                                      sizeof name), 0);

    part("sdg3");
    dm("dm-9", "CRYPT-LUKS2-c-longname\n", "longname\n");
    holds("sdg3", "dm-9");
    assert_int_equal(disk_luks_holder(g_sys, "sdg3", dev, 3u, name,
                                      sizeof name), 0);
}

int run_disk_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_open_shelf, setup, teardown),
        cmocka_unit_test_setup_teardown(test_closed, setup, teardown),
        cmocka_unit_test_setup_teardown(test_not_luks_or_two, setup,
                                        teardown),
        cmocka_unit_test_setup_teardown(test_unreadable, setup, teardown),
    };

    return cmocka_run_group_tests_name("mkstick disk", tests, NULL, NULL);
}
