/* Tests for falcon-bios/src/mp/mpcore.c: the processors a MADT lists, the
 * thread bits CPUID leaf 0Bh gives, and which processors the BIOS lane
 * runner wakes. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <mp/mpcore.h>

static uint8_t g_t[512];
static size_t g_len;

static void put32(size_t at, uint32_t v)
{
    g_t[at] = (uint8_t)v;
    g_t[at + 1] = (uint8_t)(v >> 8);
    g_t[at + 2] = (uint8_t)(v >> 16);
    g_t[at + 3] = (uint8_t)(v >> 24);
}

static void madt_begin(uint32_t lapic)
{
    memset(g_t, 0, sizeof g_t);
    memcpy(g_t, "APIC", 4);
    put32(36, lapic);
    g_len = 44;
}

static void madt_lapic(uint8_t id, uint8_t flags)
{
    g_t[g_len] = 0;
    g_t[g_len + 1] = 8;
    g_t[g_len + 2] = id;            /* the ACPI ID, unread */
    g_t[g_len + 3] = id;
    g_t[g_len + 4] = flags;
    g_len += 8;
}

static void madt_x2(uint32_t id, uint8_t flags)
{
    g_t[g_len] = 9;
    g_t[g_len + 1] = 16;
    put32(g_len + 4, id);
    g_t[g_len + 8] = flags;
    g_len += 16;
}

static void madt_end(void)
{
    put32(4, (uint32_t)g_len);
}

static void test_parse(void **state)
{
    mpb_cpu c[8];
    uint32_t lapic;
    size_t n;

    (void)state;
    madt_begin(0xFEE00000u);
    madt_lapic(0, 1);
    madt_lapic(1, 1);
    madt_lapic(2, 0);               /* disabled */
    g_t[g_len] = 1;                 /* an I/O APIC, skipped */
    g_t[g_len + 1] = 12;
    g_len += 12;
    madt_x2(1, 1);                  /* the same processor again */
    madt_x2(300, 1);
    madt_end();
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 8, &n), 0);
    assert_int_equal(lapic, 0xFEE00000u);
    assert_int_equal(n, 4);
    assert_int_equal(c[0].apic_id, 0);
    assert_int_equal(c[1].apic_id, 1);
    assert_int_equal(c[2].apic_id, 2);
    assert_int_equal(c[2].enabled, 0);
    assert_int_equal(c[3].apic_id, 300);
    assert_int_equal(c[3].enabled, 1);

    /* cap stops the list, not the parse. */
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 2, &n), 0);
    assert_int_equal(n, 2);
}

static void test_override(void **state)
{
    mpb_cpu c[4];
    uint32_t lapic;
    size_t n;

    (void)state;
    madt_begin(0xFEE00000u);
    g_t[g_len] = 5;
    g_t[g_len + 1] = 12;
    put32(g_len + 4, 0xFEC00000u);
    g_len += 12;
    madt_end();
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 4, &n), 0);
    assert_int_equal(lapic, 0xFEC00000u);
    /* Above 4 GiB: none falcon-bios can reach. */
    put32(g_len - 4, 1);
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 4, &n), 0);
    assert_int_equal(lapic, 0);
}

static void test_refusals(void **state)
{
    mpb_cpu c[4];
    uint32_t lapic;
    size_t n;

    (void)state;
    madt_begin(0xFEE00000u);
    madt_lapic(0, 1);
    madt_end();
    assert_int_equal(mpb_parse_madt(g_t, 40, &lapic, c, 4, &n), -1);
    memcpy(g_t, "FACP", 4);
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 4, &n), -1);
    memcpy(g_t, "APIC", 4);
    put32(4, (uint32_t)g_len + 1u);         /* longer than what was read */
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 4, &n), -1);
    put32(4, (uint32_t)g_len);
    g_t[45] = 1;                            /* an entry of length 1 */
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 4, &n), -1);
    g_t[45] = 60;                           /* past the table's end */
    assert_int_equal(mpb_parse_madt(g_t, g_len, &lapic, c, 4, &n), -1);
}

static void test_smt_shift(void **state)
{
    (void)state;
    assert_int_equal(mpb_smt_shift(0x10, 1, 0x100), 1);
    assert_int_equal(mpb_smt_shift(0x10, 0x21, 0x100), 1);
    assert_int_equal(mpb_smt_shift(0x0A, 1, 0x100), 0);   /* no leaf 0Bh */
    assert_int_equal(mpb_smt_shift(0x10, 4, 0x200), 0);   /* a core level */
}

/* Four cores of two threads, IDs 0..7, thread in bit 0, the boot
 * processor ID 0: one worker each on cores 1, 2 and 3. */
static void test_pick_smt(void **state)
{
    mpb_cpu c[8];
    uint32_t ids[8];
    uint32_t i;

    (void)state;
    for (i = 0; i < 8; i++) {
        c[i].apic_id = i;
        c[i].enabled = 1;
    }
    assert_int_equal(mpb_pick(c, 8, 0, 1, 15, ids), 3);
    assert_int_equal(ids[0], 2);
    assert_int_equal(ids[1], 4);
    assert_int_equal(ids[2], 6);
    /* want caps it. */
    assert_int_equal(mpb_pick(c, 8, 0, 1, 2, ids), 2);
    /* The boot processor on core 2: cores 0, 1 and 3. */
    assert_int_equal(mpb_pick(c, 8, 5, 1, 15, ids), 3);
    assert_int_equal(ids[0], 0);
    assert_int_equal(ids[1], 2);
    assert_int_equal(ids[2], 6);
    /* A first thread disabled: its core's second thread instead. */
    c[2].enabled = 0;
    assert_int_equal(mpb_pick(c, 8, 0, 1, 15, ids), 3);
    assert_int_equal(ids[0], 3);
    /* Nothing said about threads: every other processor. */
    c[2].enabled = 1;
    assert_int_equal(mpb_pick(c, 8, 0, 0, 15, ids), 7);
    assert_int_equal(mpb_pick(c, 8, 0, 32, 15, ids), 0);
    assert_int_equal(mpb_pick(c, 1, 0, 0, 15, ids), 0);
}

int run_mpcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse),
        cmocka_unit_test(test_override),
        cmocka_unit_test(test_refusals),
        cmocka_unit_test(test_smt_shift),
        cmocka_unit_test(test_pick_smt),
    };

    return cmocka_run_group_tests_name("mpcore", tests, NULL, NULL);
}
