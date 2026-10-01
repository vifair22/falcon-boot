/* Tests for falcon-bios/src/hw/usb/hidcore.c: a USB boot keyboard's reports
 * as the BIOS keys falcon jump's USB modules ring (DESIGN Section 13.4).
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <hw/usb/hidcore.h>

#define L_SHIFT 0x02u
#define R_SHIFT 0x20u
#define L_CTRL  0x01u
#define R_ALT   0x40u

static void test_keys(void **state)
{
    (void)state;
    /* What falcon-bios's menus read: Enter, Escape, the arrows. */
    assert_int_equal(hid_key(0x28, 0, 0), 0x1C0D);
    assert_int_equal(hid_key(0x29, 0, 0), 0x011B);
    assert_int_equal(hid_key(0x52, 0, 0), 0x4800);
    assert_int_equal(hid_key(0x51, 0, 0), 0x5000);
    assert_int_equal(hid_key(0x50, 0, 0), 0x4B00);
    assert_int_equal(hid_key(0x4F, 0, 0), 0x4D00);

    /* Letters, shifted either side, and Caps Lock turning Shift round. */
    assert_int_equal(hid_key(0x04, 0, 0), 0x1E61);
    assert_int_equal(hid_key(0x04, L_SHIFT, 0), 0x1E41);
    assert_int_equal(hid_key(0x1D, R_SHIFT, 0), 0x2C5A);
    assert_int_equal(hid_key(0x06, 0, KBD_CAPS), 0x2E43);
    assert_int_equal(hid_key(0x06, L_SHIFT, KBD_CAPS), 0x2E63);
    /* Caps Lock is for letters only. */
    assert_int_equal(hid_key(0x1E, 0, KBD_CAPS), 0x0231);
    assert_int_equal(hid_key(0x1E, L_SHIFT, 0), 0x0221);
    assert_int_equal(hid_key(0x27, 0, 0), 0x0B30);
    assert_int_equal(hid_key(0x38, L_SHIFT, 0), 0x353F);
    assert_int_equal(hid_key(0x2C, 0, 0), 0x3920);
    assert_int_equal(hid_key(0x2A, 0, 0), 0x0E08);
    assert_int_equal(hid_key(0x2B, 0, 0), 0x0F09);
    assert_int_equal(hid_key(0x2B, L_SHIFT, 0), 0x0F00);
    assert_int_equal(hid_key(0x32, 0, 0), 0x2B5C);
    assert_int_equal(hid_key(0x64, L_SHIFT, 0), 0x567C);

    /* Ctrl and Alt. */
    assert_int_equal(hid_key(0x06, L_CTRL, 0), 0x2E03);
    assert_int_equal(hid_key(0x28, L_CTRL, 0), 0x1C0A);
    assert_int_equal(hid_key(0x2A, L_CTRL, 0), 0x0E7F);
    assert_int_equal(hid_key(0x1F, L_CTRL, 0), 0x0300);
    assert_int_equal(hid_key(0x33, L_CTRL, 0), 0);
    assert_int_equal(hid_key(0x04, R_ALT, 0), 0x1E00);
    assert_int_equal(hid_key(0x1E, R_ALT, 0), 0x7800);
    assert_int_equal(hid_key(0x27, R_ALT, 0), 0x8100);
    assert_int_equal(hid_key(0x2D, R_ALT, 0), 0x8200);

    /* Function keys, with each modifier; F11 and F12 extended. */
    assert_int_equal(hid_key(0x3A, 0, 0), 0x3B00);
    assert_int_equal(hid_key(0x43, 0, 0), 0x4400);
    assert_int_equal(hid_key(0x44, 0, 0), 0x8500);
    assert_int_equal(hid_key(0x45, 0, 0), 0x8600);
    assert_int_equal(hid_key(0x3A, L_SHIFT, 0), 0x5400);
    assert_int_equal(hid_key(0x3A, L_CTRL, 0), 0x5E00);
    assert_int_equal(hid_key(0x3A, R_ALT, 0), 0x6800);
    assert_int_equal(hid_key(0x45, R_ALT, 0), 0x8C00);

    /* The cursor keys with Ctrl and Alt. */
    assert_int_equal(hid_key(0x4A, 0, 0), 0x4700);
    assert_int_equal(hid_key(0x4C, 0, 0), 0x5300);
    assert_int_equal(hid_key(0x50, L_CTRL, 0), 0x7300);
    assert_int_equal(hid_key(0x52, R_ALT, 0), 0x9800);

    /* The keypad: digits with Num Lock, the cursor keys without, and Shift
     * turning Num Lock round. */
    assert_int_equal(hid_key(0x60, 0, 0), 0x4800);
    assert_int_equal(hid_key(0x60, 0, KBD_NUM), 0x4838);
    assert_int_equal(hid_key(0x60, L_SHIFT, KBD_NUM), 0x4800);
    assert_int_equal(hid_key(0x62, L_SHIFT, 0), 0x5230);
    assert_int_equal(hid_key(0x63, 0, KBD_NUM), 0x532E);
    assert_int_equal(hid_key(0x59, L_CTRL, KBD_NUM), 0);
    assert_int_equal(hid_key(0x54, 0, 0), 0x352F);
    assert_int_equal(hid_key(0x57, 0, 0), 0x4E2B);
    assert_int_equal(hid_key(0x58, 0, 0), 0x1C0D);

    /* Nothing for the locks, the modifiers or what the PC has no key for. */
    assert_int_equal(hid_key(0x39, 0, 0), 0);
    assert_int_equal(hid_key(0x53, 0, 0), 0);
    assert_int_equal(hid_key(0xE1, 0, 0), 0);
    assert_int_equal(hid_key(0x65, 0, 0), 0);
    assert_int_equal(hid_key(0x00, 0, 0), 0);
}

static void test_flags(void **state)
{
    (void)state;
    assert_int_equal(hid_shift_flags(0), 0);
    assert_int_equal(hid_shift_flags(L_SHIFT), KBD_LSHIFT);
    assert_int_equal(hid_shift_flags(R_SHIFT), KBD_RSHIFT);
    assert_int_equal(hid_shift_flags(0x10), KBD_CTRL);
    assert_int_equal(hid_shift_flags(0x04 | L_CTRL), KBD_ALT | KBD_CTRL);
    assert_int_equal(hid_shift_flags(0x88), 0);   /* the GUI keys */
    assert_int_equal(hid_lock_bit(0x39), KBD_CAPS);
    assert_int_equal(hid_lock_bit(0x53), KBD_NUM);
    assert_int_equal(hid_lock_bit(0x47), KBD_SCROLL);
    assert_int_equal(hid_lock_bit(0x04), 0);
}

static void test_leds(void **state)
{
    (void)state;
    assert_int_equal(hid_leds(0), 0);
    assert_int_equal(hid_leds(KBD_NUM), 0x01);
    assert_int_equal(hid_leds(KBD_CAPS), 0x02);
    assert_int_equal(hid_leds(KBD_SCROLL), 0x04);
    assert_int_equal(hid_leds(KBD_NUM | KBD_CAPS | KBD_SCROLL), 0x07);
    /* The held keys and Insert (0x80) light nothing. */
    assert_int_equal(hid_leds(KBD_HELD | 0x80u), 0);
    assert_int_equal(hid_leds(0xFF), 0x07);
}

static void test_reports(void **state)
{
    hid_kbd_state s;
    uint8_t p[6];
    uint8_t a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
    uint8_t ab[8] = { L_SHIFT, 0, 0x04, 0x05, 0, 0, 0, 0 };
    uint8_t b[8] = { 0, 0, 0x05, 0, 0, 0, 0, 0 };
    uint8_t none[8] = { 0 };
    uint8_t roll[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
    uint8_t caps[8] = { 0, 0, 0x39, 0x05, 0, 0, 0, 0 };

    (void)state;
    memset(&s, 0, sizeof s);
    assert_int_equal(hid_report(&s, a, sizeof a, 100, p), 1);
    assert_int_equal(p[0], 0x04);
    /* Still held: nothing new. */
    assert_int_equal(hid_report(&s, a, sizeof a, 101, p), 0);
    /* B joins A: only B is new, and the modifiers are kept. */
    assert_int_equal(hid_report(&s, ab, sizeof ab, 102, p), 1);
    assert_int_equal(p[0], 0x05);
    assert_int_equal(s.mods, L_SHIFT);
    /* Too many keys: the report changes nothing. */
    assert_int_equal(hid_report(&s, roll, sizeof roll, 103, p), 0);
    assert_int_equal(s.keys[1], 0x05);
    assert_int_equal(s.mods, L_SHIFT);
    /* A released: nothing new, and B, pressed last, still repeats. */
    assert_int_equal(hid_report(&s, b, sizeof b, 104, p), 0);
    assert_int_equal(s.held, 0x05);
    assert_int_equal(hid_report(&s, none, sizeof none, 105, p), 0);
    assert_int_equal(s.held, 0);
    assert_int_equal(s.mods, 0);
    /* A lock pressed with a key: both are new, only the key repeats. */
    assert_int_equal(hid_report(&s, caps, sizeof caps, 106, p), 2);
    assert_int_equal(p[0], 0x39);
    assert_int_equal(p[1], 0x05);
    assert_int_equal(s.held, 0x05);

    /* Short reports, and none. */
    memset(&s, 0, sizeof s);
    assert_int_equal(hid_report(&s, a, 3u, 1, p), 1);
    assert_int_equal(hid_report(&s, a, 2u, 1, p), 0);
    assert_int_equal(hid_report((hid_kbd_state *)0, a, sizeof a, 1, p), 0);
}

static void test_repeat(void **state)
{
    hid_kbd_state s;
    uint8_t p[6];
    uint8_t a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
    uint8_t none[8] = { 0 };
    uint32_t t;

    (void)state;
    memset(&s, 0, sizeof s);
    assert_int_equal(hid_repeat(&s, 50), 0);
    (void)hid_report(&s, a, sizeof a, 1000, p);
    for (t = 1000; t < 1000 + HID_REPEAT_DELAY; t++)
        assert_int_equal(hid_repeat(&s, t), 0);
    assert_int_equal(hid_repeat(&s, t), 0x04);
    assert_int_equal(hid_repeat(&s, t), 0);
    assert_int_equal(hid_repeat(&s, t + HID_REPEAT_RATE - 1u), 0);
    assert_int_equal(hid_repeat(&s, t + HID_REPEAT_RATE), 0x04);
    /* A poll that comes late repeats once, not once per missed step. */
    assert_int_equal(hid_repeat(&s, t + 20u), 0x04);
    assert_int_equal(hid_repeat(&s, t + 20u), 0);

    /* Midnight: the tick count goes back, and the delay starts again. */
    assert_int_equal(hid_repeat(&s, 3), 0);
    assert_int_equal(hid_repeat(&s, 3 + HID_REPEAT_DELAY - 1u), 0);
    assert_int_equal(hid_repeat(&s, 3 + HID_REPEAT_DELAY), 0x04);

    (void)hid_report(&s, none, sizeof none, 40, p);
    assert_int_equal(hid_repeat(&s, 100), 0);
    assert_int_equal(hid_repeat((hid_kbd_state *)0, 100), 0);
}

static void set16(uint8_t *bda, unsigned off, unsigned v)
{
    bda[off] = (uint8_t)(v & 0xFFu);
    bda[off + 1u] = (uint8_t)(v >> 8);
}

static unsigned get16(const uint8_t *bda, unsigned off)
{
    return (unsigned)bda[off] | ((unsigned)bda[off + 1u] << 8);
}

static void test_ring(void **state)
{
    uint8_t bda[256];
    unsigned i;

    (void)state;
    /* The PC's ring: 0x1E-0x3E, sixteen words, one always empty. */
    memset(bda, 0, sizeof bda);
    set16(bda, 0x80, 0x1E);
    set16(bda, 0x82, 0x3E);
    set16(bda, 0x1A, 0x1E);
    set16(bda, 0x1C, 0x1E);
    assert_int_equal(kbd_ring_put(bda, 0x1C0D), 0);
    assert_int_equal(get16(bda, 0x1E), 0x1C0D);
    assert_int_equal(get16(bda, 0x1C), 0x20);
    for (i = 1; i < 15u; i++)
        assert_int_equal(kbd_ring_put(bda, (uint16_t)i), 0);
    assert_int_equal(kbd_ring_put(bda, 0x9999), -1);    /* full */
    assert_int_equal(get16(bda, 0x1C), 0x3C);

    /* The tail wraps from the ring's end to its start. */
    set16(bda, 0x1A, 0x30);
    set16(bda, 0x1C, 0x3C);
    assert_int_equal(kbd_ring_put(bda, 0x4800), 0);
    assert_int_equal(get16(bda, 0x3C), 0x4800);
    assert_int_equal(get16(bda, 0x1C), 0x1E);

    /* Bounds that describe no ring: the PC's are taken. */
    set16(bda, 0x80, 0);
    set16(bda, 0x82, 0);
    set16(bda, 0x1A, 0x1E);
    set16(bda, 0x1C, 0x22);
    assert_int_equal(kbd_ring_put(bda, 0x011B), 0);
    assert_int_equal(get16(bda, 0x22), 0x011B);
    assert_int_equal(get16(bda, 0x1C), 0x24);

    /* A ring the BIOS put elsewhere in the data area. */
    set16(bda, 0x80, 0xA0);
    set16(bda, 0x82, 0xA6);
    set16(bda, 0x1A, 0xA0);
    set16(bda, 0x1C, 0xA2);
    assert_int_equal(kbd_ring_put(bda, 0x3920), 0);
    assert_int_equal(get16(bda, 0xA2), 0x3920);
    assert_int_equal(get16(bda, 0x1C), 0xA4);
    assert_int_equal(kbd_ring_put(bda, 0x3920), -1);    /* full */

    /* A head or tail outside the ring, or a tail between two words. */
    set16(bda, 0x1A, 0x10);
    assert_int_equal(kbd_ring_put(bda, 1), -1);
    set16(bda, 0x1A, 0xA0);
    set16(bda, 0x1C, 0xA1);
    assert_int_equal(kbd_ring_put(bda, 1), -1);
    assert_int_equal(kbd_ring_put((uint8_t *)0, 1), -1);
}

int run_hidcore_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_keys),
        cmocka_unit_test(test_flags),
        cmocka_unit_test(test_leds),
        cmocka_unit_test(test_reports),
        cmocka_unit_test(test_repeat),
        cmocka_unit_test(test_ring),
    };

    return cmocka_run_group_tests_name("hidcore", tests, NULL, NULL);
}
