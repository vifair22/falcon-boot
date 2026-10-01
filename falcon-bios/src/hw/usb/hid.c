/* falcon jump's USB modules: boot-protocol keyboards, as the BIOS's own
 * keyboard (DESIGN Sections 13.4 and 13.6).
 *
 * Taking a USB controller from the BIOS ends the BIOS's keyboard emulation
 * on it (uhci.c, ohci.c; on EHCI for its companions too). This answers for
 * it: the resident's INT 16h hook (rthunk.S) calls hid_poll on the keyboard
 * calls, which reads each keyboard's interrupt endpoint once and puts what it
 * finds where the BIOS's keyboard interrupt would have: scan code and
 * character into the BIOS data area's ring, the held Shift, Ctrl and Alt
 * into its flags at 0x417. The BIOS's own INT 16h, chained to after, finds
 * them there as it finds a PS/2 key.
 *
 * The keys are hidcore.c's; this moves the report and writes the BDA. The
 * reverse thunk runs with interrupts off, so the BIOS's IRQ 1 handler
 * cannot be writing the ring at the same time.
 */

#include <hw/usb/hid.h>
#include <hw/usb/hidcore.h>
#include <hw/modrt.h>

#define KBD_MAX     4u
#define BDA         0x400u
#define BDA_FLAGS   0x17u            /* offsets from BDA */
#define BDA_TICKS   0x6Cu

typedef struct {
    unsigned      root;
    uint8_t       addr;
    uint8_t       ep;
    uint16_t      mps;
    int           low;
    uint8_t       toggle;
    hid_kbd_state st;
} hid_kbd;

static const usbhc *g_hc;
static hid_kbd g_kbd[KBD_MAX];
static unsigned g_nkbd;
/* The held-key bits of 0x417 these keyboards set, so that a PS/2
 * keyboard's are left as its interrupt handler wrote them. */
static uint8_t g_mine;
static uint8_t g_rep[HID_REPORT_LEN];

int hid_attach(const usbhc *hc, unsigned root, uint8_t addr, uint8_t mps0,
               int low, const usb_kbd_if *k)
{
    uint8_t setup[USB_SETUP_LEN];
    hid_kbd *h;

    if (g_nkbd >= KBD_MAX || k->mps == 0)
        return -1;
    /* The boot protocol is the 8-byte report hidcore.c reads. A keyboard
     * that refuses SET_IDLE reports at its own idle rate, which repeats the
     * report it last sent and so presses nothing new. */
    usb_setup_set_protocol(setup, k->iface, 0);
    (void)hc->control(root, addr, mps0, low, setup, (void *)0, 0);
    usb_setup_set_idle(setup, k->iface);
    (void)hc->control(root, addr, mps0, low, setup, (void *)0, 0);

    g_hc = hc;
    h = &g_kbd[g_nkbd++];
    h->root = root;
    h->addr = addr;
    h->ep = k->ep;
    h->mps = k->mps;
    h->low = low;
    h->toggle = 0;
    return 0;
}

unsigned hid_count(void)
{
    return g_nkbd;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void ring(uint8_t *bda, const hid_kbd *h, uint8_t usage)
{
    uint16_t key = hid_key(usage, h->st.mods, bda[BDA_FLAGS]);

    if (key != 0)
        (void)kbd_ring_put(bda, key);
}

void hid_poll(void)
{
    uint8_t *bda = low_mem(BDA);
    uint32_t now = rd32(bda + BDA_TICKS);
    uint8_t mine = 0;
    unsigned i;

    for (i = 0; i < g_nkbd; i++) {
        hid_kbd *h = &g_kbd[i];
        uint32_t len = h->mps < HID_REPORT_LEN ? h->mps : HID_REPORT_LEN;
        uint32_t done = 0;
        uint8_t pressed[6];
        uint8_t u;

        if (g_hc->intr(h->root, h->addr, h->ep, h->mps, h->low, g_rep, len,
                       &done, &h->toggle) == USBHC_OK) {
            size_t n = hid_report(&h->st, g_rep, done, now, pressed);
            size_t k;

            for (k = 0; k < n; k++) {
                uint8_t lock = hid_lock_bit(pressed[k]);

                if (lock != 0)
                    bda[BDA_FLAGS] ^= lock;
                else
                    ring(bda, h, pressed[k]);
            }
        }
        u = hid_repeat(&h->st, now);
        if (u != 0)
            ring(bda, h, u);
        mine |= hid_shift_flags(h->st.mods);
    }
    bda[BDA_FLAGS] = (uint8_t)((bda[BDA_FLAGS] & ~g_mine) | mine);
    g_mine = mine;
}
