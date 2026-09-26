/* Tests for falcon-bios/src/jump/jmod.c: the check a module passes before
 * falcon jump runs it (docs/DESIGN.md Section 14.4).
 *
 * Every refusal is a module that would have been jumped into otherwise: one
 * from another build, one linked for another slot, one whose file is cut
 * short, one that would leave no room for its stack.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <jump/jmod.h>
#include <start/biosdefs.h>

#define SLOT   JMOD_SLOT_ADDR(JMOD_SLOT_ATA)
#define FILE_LEN 0x3000u

static uint8_t g_hdr[64];

static void wr16(unsigned off, uint16_t v)
{
    g_hdr[off] = (uint8_t)(v & 0xFFu);
    g_hdr[off + 1u] = (uint8_t)(v >> 8);
}

static void wr32(unsigned off, uint32_t v)
{
    g_hdr[off] = (uint8_t)(v & 0xFFu);
    g_hdr[off + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    g_hdr[off + 2u] = (uint8_t)((v >> 16) & 0xFFu);
    g_hdr[off + 3u] = (uint8_t)(v >> 24);
}

/* ATA.MOD as the build lays it out: 12 KB of image, 4 KB of .bss. */
static void good(void)
{
    memset(g_hdr, 0, sizeof g_hdr);
    wr32(JMOD_OFF_MAGIC, JMOD_MAGIC);
    wr16(JMOD_OFF_ABI, JMOD_ABI);
    wr16(JMOD_OFF_HDR_LEN, JMOD_HDR_LEN);
    wr32(JMOD_OFF_SLOT, SLOT);
    wr32(JMOD_OFF_IMAGE_END, SLOT + FILE_LEN);
    wr32(JMOD_OFF_MEM_END, SLOT + FILE_LEN + 0x1000u);
    wr32(JMOD_OFF_INIT, SLOT + 0x40u);
    memcpy(g_hdr + JMOD_OFF_NAME, "ATA", 3);
}

static jmod_status check(jmod_info *i)
{
    return jmod_check(g_hdr, sizeof g_hdr, FILE_LEN, SLOT, JMOD_SLOT_BYTES, i);
}

static void test_good(void **state)
{
    jmod_info i;

    (void)state;
    good();
    assert_int_equal(check(&i), JMOD_OK);
    assert_int_equal(i.slot, SLOT);
    assert_int_equal(i.image_len, FILE_LEN);
    assert_int_equal(i.mem_len, FILE_LEN + 0x1000u);
    assert_int_equal(i.init, SLOT + 0x40u);
    assert_string_equal(i.name, "ATA");
}

static void test_refusals(void **state)
{
    jmod_info i;

    (void)state;
    good();
    wr32(JMOD_OFF_MAGIC, 0x464C457Fu);           /* an ELF, say */
    assert_int_equal(check(&i), JMOD_ERR_MAGIC);

    good();
    wr16(JMOD_OFF_ABI, JMOD_ABI + 1u);
    assert_int_equal(check(&i), JMOD_ERR_ABI);

    good();
    wr32(JMOD_OFF_SLOT, JMOD_SLOT_ADDR(JMOD_SLOT_UHCI));
    assert_int_equal(check(&i), JMOD_ERR_SLOT);

    /* A file cut short on the floppy. */
    good();
    assert_int_equal(jmod_check(g_hdr, sizeof g_hdr, FILE_LEN - 512u, SLOT,
                                JMOD_SLOT_BYTES, &i), JMOD_ERR_SIZE);

    /* .bss reaching into the stack at the slot's top. */
    good();
    wr32(JMOD_OFF_MEM_END, SLOT + JMOD_SLOT_BYTES - JMOD_STACK_BYTES + 1u);
    assert_int_equal(check(&i), JMOD_ERR_SIZE);
    wr32(JMOD_OFF_MEM_END, SLOT + JMOD_SLOT_BYTES - JMOD_STACK_BYTES);
    assert_int_equal(check(&i), JMOD_OK);

    good();
    wr32(JMOD_OFF_MEM_END, SLOT + FILE_LEN - 1u);
    assert_int_equal(check(&i), JMOD_ERR_SIZE);

    /* An entry in the header, or past the image. */
    good();
    wr32(JMOD_OFF_INIT, SLOT);
    assert_int_equal(check(&i), JMOD_ERR_ENTRY);
    wr32(JMOD_OFF_INIT, SLOT + FILE_LEN);
    assert_int_equal(check(&i), JMOD_ERR_ENTRY);

    good();
    assert_int_equal(jmod_check(g_hdr, 16u, FILE_LEN, SLOT, JMOD_SLOT_BYTES,
                                &i), JMOD_ERR_MAGIC);
    assert_int_equal(jmod_check(NULL, 64u, FILE_LEN, SLOT, JMOD_SLOT_BYTES,
                                &i), JMOD_ERR_ARG);
    assert_int_equal(jmod_check(g_hdr, 64u, FILE_LEN, SLOT, 0x1000u, &i),
                     JMOD_ERR_ARG);
}

/* A name that is not text is cut at the first byte that is not. */
static void test_name(void **state)
{
    jmod_info i;

    (void)state;
    good();
    memcpy(g_hdr + JMOD_OFF_NAME, "UHCI\x01xyz", 8);
    assert_int_equal(check(&i), JMOD_OK);
    assert_string_equal(i.name, "UHCI");
}

int run_jmod_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_good),
        cmocka_unit_test(test_refusals),
        cmocka_unit_test(test_name),
    };

    return cmocka_run_group_tests_name("jmod", tests, NULL, NULL);
}
