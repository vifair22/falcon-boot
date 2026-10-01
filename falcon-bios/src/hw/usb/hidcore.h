/* falcon jump's USB modules: a boot-protocol keyboard's reports as the
 * BIOS's keyboard would have delivered them (DESIGN Section 13.4).
 *
 * What a key press becomes is a scan code and a character in the BIOS's
 * keyboard ring, which is what every INT 16h caller reads: falcon-bios's
 * menus, DOS, a medium's bootloader. A wrong entry here is a key that does
 * something else or nothing, with no error anywhere, so the translation, the
 * report decoding, the repeat and the ring are here, host-tested; hid.c moves
 * the reports and writes the results into the BDA.
 */

#ifndef FALCON_BIOS_HIDCORE_H
#define FALCON_BIOS_HIDCORE_H

#include <stddef.h>
#include <stdint.h>

#define HID_REPORT_LEN 8u

/* The BDA's shift flags at 0x417. */
#define KBD_RSHIFT 0x01u
#define KBD_LSHIFT 0x02u
#define KBD_CTRL   0x04u
#define KBD_ALT    0x08u
#define KBD_SCROLL 0x10u
#define KBD_NUM    0x20u
#define KBD_CAPS   0x40u
#define KBD_HELD   (KBD_RSHIFT | KBD_LSHIFT | KBD_CTRL | KBD_ALT)

/* The repeat, in BIOS ticks (18.2 a second): the first after about half a
 * second, then about nine a second, the PC keyboard's defaults. */
#define HID_REPEAT_DELAY 9u
#define HID_REPEAT_RATE  2u

/* The report's modifier byte as the BDA's held-key bits. */
uint8_t hid_shift_flags(uint8_t mods);

/* The lock a usage toggles (KBD_CAPS, KBD_NUM, KBD_SCROLL), or 0. */
uint8_t hid_lock_bit(uint8_t usage);

/* Usage `usage` (HID usage page 7) with modifier byte `mods` and the BDA's
 * shift flags `flags` (the locks): the INT 16h word, scan code high and
 * character low, as the BIOS would ring it for the same key on a US
 * keyboard. 0 for a usage that rings nothing (a lock, a modifier, a key the
 * PC keyboard has no code for in that combination). */
uint16_t hid_key(uint8_t usage, uint8_t mods, uint8_t flags);

typedef struct {
    uint8_t  keys[6];        /* the usages the last report held */
    uint8_t  mods;
    uint8_t  held;           /* the key that repeats, or 0 */
    uint32_t last;           /* the tick count when last asked */
    uint32_t next;           /* the tick of the next repeat */
} hid_kbd_state;

/* Takes report `rep` at tick `now`: the usages it holds that the last one
 * did not, in report order, into `pressed`, and how many. A report of
 * ErrorRollOver (too many keys) changes nothing. The last key pressed is the
 * one that repeats, until it is released. */
size_t hid_report(hid_kbd_state *s, const uint8_t *rep, size_t len,
                  uint32_t now, uint8_t pressed[6]);

/* The usage to ring again at tick `now`, or 0. The count running backwards
 * (the BIOS clears it at midnight) restarts the delay. */
uint8_t hid_repeat(hid_kbd_state *s, uint32_t now);

/* Puts `key` at the tail of the BIOS's keyboard ring. `bda` is the 256
 * bytes of the BIOS data area (0x400 on the machine): head and tail at
 * 0x1A and 0x1C, the ring's bounds at 0x80 and 0x82, offsets from 0x400,
 * with the PC's 0x1E-0x3E when those do not describe a ring. 0, or -1 when
 * the ring is full or its head and tail are not inside it. */
int kbd_ring_put(uint8_t *bda, uint16_t key);

#endif /* FALCON_BIOS_HIDCORE_H */
