/* falcon jump's USB modules: boot keyboard reports as BIOS keys. See
 * hidcore.h.
 *
 * The codes are the IBM PC BIOS's (scan code set 1, and the extended codes
 * INT 16h AH=10h returns for F11, F12 and the Ctrl and Alt combinations).
 * Where the PC keyboard's grey cursor keys ring E0h as their character, this
 * rings 00h, as the numeric keypad's do: AH=00h callers, falcon-bios's
 * bios_getkey among them, know an arrow by a zero character and its scan
 * code.
 */

#include <hw/usb/hidcore.h>

/* HID modifier byte: left Ctrl, Shift, Alt, GUI, then the right ones. */
#define MOD_CTRL  0x11u
#define MOD_SHIFT 0x22u
#define MOD_ALT   0x44u

#define U_A        0x04u
#define U_Z        0x1Du
#define U_1        0x1Eu
#define U_0        0x27u
#define U_ENTER    0x28u
#define U_ESC      0x29u
#define U_BKSP     0x2Au
#define U_TAB      0x2Bu
#define U_SPACE    0x2Cu
#define U_MINUS    0x2Du
#define U_EQUAL    0x2Eu
#define U_LBRACKET 0x2Fu
#define U_RBRACKET 0x30u
#define U_BSLASH   0x31u
#define U_SLASH    0x38u
#define U_CAPS     0x39u
#define U_F1       0x3Au
#define U_F12      0x45u
#define U_SCROLL   0x47u
#define U_INSERT   0x49u
#define U_UP       0x52u
#define U_NUM      0x53u
#define U_KP_SLASH 0x54u
#define U_KP_ENTER 0x58u
#define U_KP_1     0x59u
#define U_KP_DOT   0x63u
#define U_NONUS_BS 0x64u
#define U_ROLLOVER 0x01u

typedef struct {
    uint8_t scan;
    uint8_t lo;              /* the character unshifted */
    uint8_t hi;              /* and shifted */
} key3;

/* Usages 0x04 (A) to 0x38 (slash): the typing keys. */
static const key3 k_main[U_SLASH - U_A + 1u] = {
    { 0x1E, 'a', 'A' }, { 0x30, 'b', 'B' }, { 0x2E, 'c', 'C' },
    { 0x20, 'd', 'D' }, { 0x12, 'e', 'E' }, { 0x21, 'f', 'F' },
    { 0x22, 'g', 'G' }, { 0x23, 'h', 'H' }, { 0x17, 'i', 'I' },
    { 0x24, 'j', 'J' }, { 0x25, 'k', 'K' }, { 0x26, 'l', 'L' },
    { 0x32, 'm', 'M' }, { 0x31, 'n', 'N' }, { 0x18, 'o', 'O' },
    { 0x19, 'p', 'P' }, { 0x10, 'q', 'Q' }, { 0x13, 'r', 'R' },
    { 0x1F, 's', 'S' }, { 0x14, 't', 'T' }, { 0x16, 'u', 'U' },
    { 0x2F, 'v', 'V' }, { 0x11, 'w', 'W' }, { 0x2D, 'x', 'X' },
    { 0x15, 'y', 'Y' }, { 0x2C, 'z', 'Z' },
    { 0x02, '1', '!' }, { 0x03, '2', '@' }, { 0x04, '3', '#' },
    { 0x05, '4', '$' }, { 0x06, '5', '%' }, { 0x07, '6', '^' },
    { 0x08, '7', '&' }, { 0x09, '8', '*' }, { 0x0A, '9', '(' },
    { 0x0B, '0', ')' },
    { 0x1C, 0x0D, 0x0D }, { 0x01, 0x1B, 0x1B }, { 0x0E, 0x08, 0x08 },
    { 0x0F, 0x09, 0x00 }, { 0x39, ' ', ' ' },
    { 0x0C, '-', '_' }, { 0x0D, '=', '+' }, { 0x1A, '[', '{' },
    { 0x1B, ']', '}' }, { 0x2B, '\\', '|' },
    { 0x2B, '\\', '|' },     /* non-US # and ~, where a US board has \ */
    { 0x27, ';', ':' }, { 0x28, '\'', '"' }, { 0x29, '`', '~' },
    { 0x33, ',', '<' }, { 0x34, '.', '>' }, { 0x35, '/', '?' },
};

/* Usages 0x49 (Insert) to 0x52 (Up): the editing and cursor keys, plain,
 * with Ctrl and with Alt. */
static const uint8_t k_nav[U_UP - U_INSERT + 1u][3] = {
    { 0x52, 0x92, 0xA2 },    /* Insert */
    { 0x47, 0x77, 0x97 },    /* Home */
    { 0x49, 0x84, 0x99 },    /* Page Up */
    { 0x53, 0x93, 0xA3 },    /* Delete */
    { 0x4F, 0x75, 0x9F },    /* End */
    { 0x51, 0x76, 0xA1 },    /* Page Down */
    { 0x4D, 0x74, 0x9D },    /* Right */
    { 0x4B, 0x73, 0x9B },    /* Left */
    { 0x50, 0x91, 0xA0 },    /* Down */
    { 0x48, 0x8D, 0x98 },    /* Up */
};

/* Usages 0x59 (keypad 1) to 0x63 (keypad .): the scan code, which is the
 * cursor key's, and the character with Num Lock. */
static const uint8_t k_pad[U_KP_DOT - U_KP_1 + 1u][2] = {
    { 0x4F, '1' }, { 0x50, '2' }, { 0x51, '3' }, { 0x4B, '4' },
    { 0x4C, '5' }, { 0x4D, '6' }, { 0x47, '7' }, { 0x48, '8' },
    { 0x49, '9' }, { 0x52, '0' }, { 0x53, '.' },
};

/* Usages 0x54-0x57: keypad / * - +. */
static const uint8_t k_pad_ops[4][2] = {
    { 0x35, '/' }, { 0x37, '*' }, { 0x4A, '-' }, { 0x4E, '+' },
};

static uint16_t word(uint8_t scan, uint8_t ch)
{
    return (uint16_t)(((unsigned)scan << 8) | ch);
}

uint8_t hid_shift_flags(uint8_t mods)
{
    uint8_t f = 0;

    if ((mods & 0x20u) != 0)
        f |= KBD_RSHIFT;
    if ((mods & 0x02u) != 0)
        f |= KBD_LSHIFT;
    if ((mods & MOD_CTRL) != 0)
        f |= KBD_CTRL;
    if ((mods & MOD_ALT) != 0)
        f |= KBD_ALT;
    return f;
}

uint8_t hid_lock_bit(uint8_t usage)
{
    switch (usage) {
    case U_CAPS:   return KBD_CAPS;
    case U_NUM:    return KBD_NUM;
    case U_SCROLL: return KBD_SCROLL;
    default:       return 0;
    }
}

/* Ctrl with a typing key: a letter's control character, and the few others
 * the PC keyboard gives one. */
static uint16_t ctrl_key(uint8_t usage, const key3 *k)
{
    if (usage >= U_A && usage <= U_Z)
        return word(k->scan, (uint8_t)(k->lo - 0x60u));
    switch (usage) {
    case U_ENTER:    return word(k->scan, 0x0A);
    case U_ESC:      return word(k->scan, 0x1B);
    case U_BKSP:     return word(k->scan, 0x7F);
    case U_TAB:      return word(0x94, 0x00);
    case U_SPACE:    return word(k->scan, ' ');
    case U_LBRACKET: return word(k->scan, 0x1B);
    case U_RBRACKET: return word(k->scan, 0x1D);
    case U_BSLASH:   return word(k->scan, 0x1C);
    case U_MINUS:    return word(k->scan, 0x1F);
    case U_1 + 1u:   return word(k->scan, 0x00);      /* Ctrl-2: NUL */
    case U_1 + 5u:   return word(k->scan, 0x1E);      /* Ctrl-6 */
    default:         return 0;
    }
}

/* Alt with a typing key: no character, and the digit row's own codes. */
static uint16_t alt_key(uint8_t usage, const key3 *k)
{
    if (usage >= U_1 && usage <= U_0)
        return word((uint8_t)(0x78u + (usage - U_1)), 0);
    switch (usage) {
    case U_MINUS: return word(0x82, 0);
    case U_EQUAL: return word(0x83, 0);
    case U_TAB:   return word(0xA5, 0);
    default:      return word(k->scan, 0);
    }
}

uint16_t hid_key(uint8_t usage, uint8_t mods, uint8_t flags)
{
    int shift = (mods & MOD_SHIFT) != 0;
    int ctrl = (mods & MOD_CTRL) != 0;
    int alt = (mods & MOD_ALT) != 0;

    if (usage >= U_A && usage <= U_SLASH) {
        const key3 *k = &k_main[usage - U_A];
        int upper = shift;

        if (alt)
            return alt_key(usage, k);
        if (ctrl)
            return ctrl_key(usage, k);
        if (usage <= U_Z && (flags & KBD_CAPS) != 0)
            upper = !upper;
        if (usage == U_TAB && shift)
            return word(k->scan, 0);
        return word(k->scan, upper ? k->hi : k->lo);
    }
    if (usage >= U_F1 && usage <= U_F12) {
        unsigned i = usage - U_F1;
        static const uint8_t f1112[4][2] = {
            { 0x85, 0x86 }, { 0x87, 0x88 }, { 0x89, 0x8A }, { 0x8B, 0x8C },
        };
        static const uint8_t f1[4] = { 0x3B, 0x54, 0x5E, 0x68 };
        unsigned m = alt ? 3u : ctrl ? 2u : shift ? 1u : 0u;

        return word(i < 10u ? (uint8_t)(f1[m] + i) : f1112[m][i - 10u], 0);
    }
    if (usage >= U_INSERT && usage <= U_UP)
        return word(k_nav[usage - U_INSERT][alt ? 2 : ctrl ? 1 : 0], 0);
    if (usage >= U_KP_SLASH && usage < U_KP_ENTER) {
        const uint8_t *o = k_pad_ops[usage - U_KP_SLASH];

        return word(o[0], alt ? 0 : o[1]);
    }
    if (usage == U_KP_ENTER)
        return word(0x1C, alt ? 0x00 : ctrl ? 0x0A : 0x0D);
    if (usage >= U_KP_1 && usage <= U_KP_DOT) {
        const uint8_t *p = k_pad[usage - U_KP_1];

        /* Alt with the keypad digits types a character by its code, and
         * Ctrl gives the cursor keys' Ctrl codes; neither is a key a boot
         * menu takes, so they ring nothing. */
        if (alt || ctrl)
            return 0;
        if (((flags & KBD_NUM) != 0) != shift)
            return word(p[0], p[1]);
        return word(p[0], 0);
    }
    if (usage == U_NONUS_BS)
        return word(0x56, alt ? 0 : shift ? '|' : '\\');
    return 0;
}

static int holds(const uint8_t keys[6], uint8_t usage)
{
    size_t i;

    for (i = 0; i < 6u; i++) {
        if (keys[i] == usage)
            return 1;
    }
    return 0;
}

size_t hid_report(hid_kbd_state *s, const uint8_t *rep, size_t len,
                  uint32_t now, uint8_t pressed[6])
{
    uint8_t keys[6];
    size_t n = 0;
    size_t i;

    if (s == (hid_kbd_state *)0 || rep == (const uint8_t *)0 || len < 3u)
        return 0;
    for (i = 0; i < 6u; i++)
        keys[i] = (2u + i < len) ? rep[2u + i] : 0;
    if (keys[0] == U_ROLLOVER)
        return 0;
    for (i = 0; i < 6u; i++) {
        uint8_t u = keys[i];

        if (u < U_A || holds(s->keys, u) || (u >= 0xE0u && u <= 0xE7u))
            continue;
        pressed[n++] = u;
        if (hid_lock_bit(u) == 0) {
            s->held = u;
            s->next = now + HID_REPEAT_DELAY;
        }
    }
    if (s->held != 0 && !holds(keys, s->held))
        s->held = 0;
    for (i = 0; i < 6u; i++)
        s->keys[i] = keys[i];
    s->mods = rep[0];
    s->last = now;
    return n;
}

uint8_t hid_repeat(hid_kbd_state *s, uint32_t now)
{
    if (s == (hid_kbd_state *)0 || s->held == 0)
        return 0;
    if (now < s->last)
        s->next = now + HID_REPEAT_DELAY;
    s->last = now;
    if (now < s->next)
        return 0;
    s->next = now + HID_REPEAT_RATE;
    return s->held;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

int kbd_ring_put(uint8_t *bda, uint16_t key)
{
    uint16_t start;
    uint16_t end;
    uint16_t head;
    uint16_t tail;
    uint16_t next;

    if (bda == (uint8_t *)0)
        return -1;
    start = rd16(bda + 0x80);
    end = rd16(bda + 0x82);
    if (start < 0x1Eu || end <= start || end > 0x100u) {
        start = 0x1Eu;
        end = 0x3Eu;
    }
    head = rd16(bda + 0x1A);
    tail = rd16(bda + 0x1C);
    if (head < start || head >= end || tail < start || tail >= end ||
        ((tail - start) & 1u) != 0)
        return -1;
    next = (uint16_t)(tail + 2u);
    if (next >= end)
        next = start;
    if (next == head)
        return -1;
    wr16(bda + tail, key);
    wr16(bda + 0x1C, next);
    return 0;
}
