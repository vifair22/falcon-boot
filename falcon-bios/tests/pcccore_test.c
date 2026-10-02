/* Tests for falcon-bios/src/hw/pcc/pcccore.c: a PC Card's CIS and the ExCA
 * window registers falcon jump's PCC.MOD programs (DESIGN Section 13.4).
 *
 * The CIS is the one falcon-bios/tools/qemu-pcc/falcon-pcc.c gives the rig's
 * CompactFlash card, which is the CF standard's shape: a memory-mapped
 * default entry, the contiguous-I/O entry, and the primary IDE range.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <hw/pcc/pcccore.h>

static const uint8_t k_cis[] = {
    0x01, 0x03, 0xD9, 0x01, 0xFF,                         /* DEVICE */
    0x21, 0x02, 0x04, 0x01,                               /* FUNCID disk */
    0x1A, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0F,             /* CONFIG 0x200 */
    0x1B, 0x09, 0xC0, 0xC0, 0x21, 0x21, 0xB5, 0x1E, 0x4D, 0x08, 0x00,
    0x1B, 0x07, 0x01, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0x64,
    0x1B, 0x0E, 0x02, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0xEA, 0x61, 0xF0, 0x01,
    0x07, 0xF6, 0x03, 0x01,
    0xFF
};

static void test_cf_card(void **state)
{
    pcc_cis c;

    (void)state;
    assert_int_equal(pcc_cis_parse(k_cis, sizeof k_cis, &c), 0);
    assert_int_equal(c.funcid, FUNCID_FIXED_DISK);
    assert_int_equal(c.config_base, 0x200u);
    /* The contiguous-I/O entry, not the memory-mapped default nor the
     * fixed primary range. */
    assert_int_equal(c.index, 1);
    assert_int_equal(c.io_lines, 4);
    assert_int_equal(c.io_len, 0);
}

/* A card offering only the fixed range: that range is taken. */
static void test_fixed_only(void **state)
{
    static const uint8_t cis[] = {
        0x21, 0x02, 0x04, 0x01,
        0x1A, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0F,
        0x1B, 0x0E, 0x02, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0xEA, 0x61, 0xF0,
        0x01, 0x07, 0xF6, 0x03, 0x01,
        0xFF
    };
    pcc_cis c;

    (void)state;
    assert_int_equal(pcc_cis_parse(cis, sizeof cis, &c), 0);
    assert_int_equal(c.index, 2);
    assert_int_equal(c.io_lines, 10);
    assert_int_equal(c.io_base, 0x1F0);
    assert_int_equal(c.io_len, 8);
}

static void test_refusals(void **state)
{
    static const uint8_t no_config[] = {
        0x21, 0x02, 0x04, 0x01,
        0x1B, 0x07, 0x01, 0x09, 0x21, 0xB5, 0x1E, 0x4D, 0x64,
        0xFF
    };
    static const uint8_t no_io[] = {
        0x1A, 0x05, 0x01, 0x03, 0x00, 0x02, 0x0F,
        0x1B, 0x09, 0xC0, 0xC0, 0x21, 0x21, 0xB5, 0x1E, 0x4D, 0x08, 0x00,
        0xFF
    };
    uint8_t cut[20];
    pcc_cis c;

    (void)state;
    assert_int_equal(pcc_cis_parse(no_config, sizeof no_config, &c), -1);
    assert_int_equal(pcc_cis_parse(no_io, sizeof no_io, &c), -1);
    /* A tuple whose length runs past the end. */
    memcpy(cut, k_cis, sizeof cut);
    assert_int_equal(pcc_cis_parse(cut, sizeof cut, &c), -1);
    assert_int_equal(pcc_cis_parse(NULL, 4u, &c), -1);
}

static void test_windows(void **state)
{
    uint8_t m[6];
    uint8_t io[4];

    (void)state;
    /* 4 KB of attribute memory at card 0, seen at host 0xD0000: the offset
     * is -0xD0 pages, modulo 2^14. */
    assert_int_equal(pcc_mem_window(0xD0000u, 0x1000u, 0, 1, m), 0);
    assert_int_equal(m[0], 0xD0);
    assert_int_equal(m[1], 0x00);
    assert_int_equal(m[2], 0xD0);
    assert_int_equal(m[3], 0x00);
    assert_int_equal((m[4] | ((m[5] & 0x3Fu) << 8)), (0x4000u - 0xD0u));
    assert_int_equal(m[5] & 0x40u, 0x40u);
    /* Card = host: a zero offset. */
    assert_int_equal(pcc_mem_window(0xC8000u, 0x2000u, 0xC8000u, 0, m), 0);
    assert_int_equal(m[2], 0xC9);
    assert_int_equal(m[4], 0);
    assert_int_equal(m[5], 0);
    assert_int_equal(pcc_mem_window(0xD0800u, 0x1000u, 0, 1, m), -1);
    assert_int_equal(pcc_mem_window(0xFFF000u, 0x2000u, 0, 1, m), -1);
    assert_int_equal(pcc_mem_window(0xD0000u, 0, 0, 1, m), -1);

    assert_int_equal(pcc_io_window(0x1E0u, 16u, io), 0);
    assert_int_equal(io[0], 0xE0);
    assert_int_equal(io[1], 0x01);
    assert_int_equal(io[2], 0xEF);
    assert_int_equal(io[3], 0x01);
    assert_int_equal(pcc_io_window(0xFFF8u, 16u, io), -1);
    assert_int_equal(pcc_io_window(0x1E0u, 0, io), -1);
}

/* An option ROM of `size` bytes (its size byte, 512-byte units) at `at`. */
static void rom_at(uint8_t *hdr, uint32_t at, uint32_t size)
{
    uint8_t *h = hdr + 3u * ((at - PCC_ROM_BASE) / PCC_ROM_SLOT);

    h[0] = 0x55;
    h[1] = 0xAA;
    h[2] = (uint8_t)(size / 512u);
}

static void test_pick(void **state)
{
    uint8_t hdr[3u * PCC_ROM_SLOTS];
    uint32_t a;

    (void)state;
    /* Nothing: the first candidate, above the video BIOS's 32 KB. */
    memset(hdr, 0xFF, sizeof hdr);
    assert_int_equal(pcc_attr_pick(hdr, 0), 0xC8000);
    /* The video BIOS, then the A21m's 6 KB boot agent at 0xD0000: the
     * candidates between are free, and the one at 0xD0000 skips to the
     * first 4 KB past the agent's last 2 KB. */
    rom_at(hdr, 0xC0000u, 0x8000u);
    assert_int_equal(pcc_attr_pick(hdr, 0), 0xC8000);
    rom_at(hdr, 0xD0000u, 6u * 1024u);
    assert_int_equal(pcc_attr_pick(hdr, 0xD0000u), 0xD2000);
    /* A later `from` starts the search there, rounded up to 4 KB. */
    assert_int_equal(pcc_attr_pick(hdr, 0xD2001u), 0xD3000);
    /* A ROM at a 2 KB boundary takes the 4 KB page it is in. */
    memset(hdr, 0xFF, sizeof hdr);
    rom_at(hdr, 0xC8800u, 2048u);
    assert_int_equal(pcc_attr_pick(hdr, 0), 0xC9000);
    /* A ROM starting below a candidate whose size runs past its start:
     * 0xC8000 for 9 KB covers 0xCA000's first bytes. */
    memset(hdr, 0xFF, sizeof hdr);
    rom_at(hdr, 0xC8000u, 9u * 1024u);
    assert_int_equal(pcc_attr_pick(hdr, 0), 0xCB000);
    /* A size byte of 0 still claims its header's 2 KB. */
    memset(hdr, 0xFF, sizeof hdr);
    rom_at(hdr, 0xC8000u, 0);
    assert_int_equal(pcc_attr_pick(hdr, 0), 0xC9000);
    /* ROMs throughout: nothing. */
    for (a = PCC_ROM_BASE; a < PCC_ROM_END; a += 0x8000u)
        rom_at(hdr, a, 0x8000u);
    assert_int_equal(pcc_attr_pick(hdr, 0), -1);
    /* The last page is 0xDF000; nothing past it. */
    memset(hdr, 0xFF, sizeof hdr);
    assert_int_equal(pcc_attr_pick(hdr, 0xDE001u), 0xDF000);
    assert_int_equal(pcc_attr_pick(hdr, 0xDF001u), -1);
    assert_int_equal(pcc_attr_pick(hdr, 0xFFFFFFFFu), -1);
    assert_int_equal(pcc_attr_pick(NULL, 0), -1);
}

/* A free ISA range reads 0xFF on every port; one port answering otherwise
 * is enough to pass it over. */
static void test_io_free(void **state)
{
    uint8_t p[PCC_IO_PORTS];

    (void)state;
    memset(p, 0xFF, sizeof p);
    assert_int_equal(pcc_io_free(p), 1);
    /* A third IDE channel's status register at base + 7. */
    p[7] = 0x50;
    assert_int_equal(pcc_io_free(p), 0);
    memset(p, 0xFF, sizeof p);
    p[PCC_IO_PORTS - 1u] = 0x7F;
    assert_int_equal(pcc_io_free(p), 0);
    memset(p, 0x00, sizeof p);
    assert_int_equal(pcc_io_free(p), 0);
    assert_int_equal(pcc_io_free(NULL), 0);
}

/* The page for a socket's registers: the highest under the APICs that
 * nothing on the bus decodes, as SeaBIOS fills the top of that region
 * with the small BARs it places last. */
static void test_bar_pick(void **state)
{
    pcc_range used[4];
    const uint64_t ram = 128u * 1024u * 1024u;

    (void)state;
    /* Nothing on the bus: the top page. */
    assert_int_equal(pcc_bar_pick(NULL, 0, ram), 0xFEBFF000u);
    /* A VGA's 4 KB at the top and a NIC's 128 KB under it: the page
     * between them is the first free. */
    used[0].base = 0xFEBFF000u; used[0].len = 0x1000u;
    used[1].base = 0xFEB80000u; used[1].len = 0x20000u;
    assert_int_equal(pcc_bar_pick(used, 2, ram), 0xFEBFE000u);
    /* The NIC moved up against the VGA: under both. */
    used[1].base = 0xFEBDF000u;
    assert_int_equal(pcc_bar_pick(used, 2, ram), 0xFEBDE000u);
    /* A range that is not page aligned takes both pages it touches. */
    used[2].base = 0xFEBDE800u; used[2].len = 0x100u;
    assert_int_equal(pcc_bar_pick(used, 3, ram), 0xFEBDD000u);
    /* A 16 MB framebuffer from 0xF0000000 leaves the rest. */
    used[3].base = 0xF0000000u; used[3].len = 0x1000000u;
    assert_int_equal(pcc_bar_pick(used, 4, ram), 0xFEBDD000u);
    /* The whole region decoded: nothing. */
    used[0].base = 0xF0000000u; used[0].len = 0x0EC00000u;
    assert_int_equal(pcc_bar_pick(used, 1, ram), 0);
    /* A window that ends at 4 GB exactly covers the region too. */
    used[0].base = 0xE0000000u; used[0].len = 0x20000000u;
    assert_int_equal(pcc_bar_pick(used, 1, ram), 0);
    /* Ranges above the region or ending at its start do not count. */
    used[0].base = 0xFEC00000u; used[0].len = 0x1000u;
    used[1].base = 0xEF000000u; used[1].len = 0x01000000u;
    assert_int_equal(pcc_bar_pick(used, 2, ram), 0xFEBFF000u);
    /* An empty range counts for nothing either. */
    used[0].base = 0xFEBFF000u; used[0].len = 0;
    assert_int_equal(pcc_bar_pick(used, 1, ram), 0xFEBFF000u);
    /* RAM reaching into the region: nothing below its top, rounded up. */
    assert_int_equal(pcc_bar_pick(NULL, 0, 0xFEBFF000u), 0xFEBFF000u);
    assert_int_equal(pcc_bar_pick(NULL, 0, 0xFEBFE001u), 0xFEBFF000u);
    used[0].base = 0xFEBFF000u; used[0].len = 0x1000u;
    assert_int_equal(pcc_bar_pick(used, 1, 0xFEBFE001u), 0);
    assert_int_equal(pcc_bar_pick(NULL, 0, 0xFEBFF001u), 0);
    assert_int_equal(pcc_bar_pick(NULL, 0, 0xFEC00000u), 0);
    assert_int_equal(pcc_bar_pick(NULL, 0, 0x100000000ull), 0);
    assert_int_equal(pcc_bar_pick(NULL, 0, 0), 0xFEBFF000u);
    assert_int_equal(pcc_bar_pick(NULL, 1, ram), 0);
}

int run_pcccore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cf_card),
        cmocka_unit_test(test_fixed_only),
        cmocka_unit_test(test_refusals),
        cmocka_unit_test(test_windows),
        cmocka_unit_test(test_pick),
        cmocka_unit_test(test_io_free),
        cmocka_unit_test(test_bar_pick),
    };

    return cmocka_run_group_tests_name("pcccore", tests, NULL, NULL);
}
