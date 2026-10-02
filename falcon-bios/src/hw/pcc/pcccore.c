/* falcon jump's PCC.MOD: CIS and ExCA windows. See pcccore.h. */

#include <hw/pcc/pcccore.h>

/* The width a descriptor's two-bit size code names: 0, 1, 2 or 4 bytes. */
static const unsigned k_widths[4] = { 0, 1, 2, 4 };

/* A multi-byte field of that width, little endian. */
static uint32_t field(const uint8_t *p, unsigned code, const uint8_t *end,
                      int *ok)
{
    unsigned w = k_widths[code & 3u];
    uint32_t v = 0;
    unsigned i;

    if (p + w > end) {
        *ok = 0;
        return 0;
    }
    for (i = 0; i < w; i++)
        v |= (uint32_t)p[i] << (8u * i);
    return v;
}

/* Skips a power descriptor: a parameter selection byte, then one value per
 * bit set in it, each value continued while its byte has bit 7 set. */
static const uint8_t *skip_power(const uint8_t *p, const uint8_t *end)
{
    uint8_t sel;
    unsigned bit;

    if (p >= end)
        return (const uint8_t *)0;
    sel = *p++;
    for (bit = 0; bit < 7u; bit++) {
        if ((sel & (1u << bit)) == 0)
            continue;
        do {
            if (p >= end)
                return (const uint8_t *)0;
        } while ((*p++ & 0x80u) != 0);
    }
    return p;
}

/* One CISTPL_CFTABLE_ENTRY body: its index, and whether and how it wants
 * I/O. 1 when it wants I/O the way pcc_cis_parse can use, with `into`
 * filled; 0 when not; -1 when it is malformed. */
static int cftable(const uint8_t *p, const uint8_t *end, pcc_cis *into)
{
    uint8_t index;
    uint8_t features;
    unsigned i;
    int ok = 1;

    if (p >= end)
        return -1;
    index = (uint8_t)(*p & 0x3Fu);
    if ((*p++ & 0x80u) != 0) {       /* an interface description byte */
        if (p >= end)
            return -1;
        p++;
    }
    if (p >= end)
        return 0;
    features = *p++;
    for (i = 0; i < (features & 3u); i++) {
        p = skip_power(p, end);
        if (p == (const uint8_t *)0)
            return -1;
    }
    if ((features & 0x04u) != 0) {   /* timing */
        uint8_t t;

        if (p >= end)
            return -1;
        t = *p++;
        /* WAIT, READY and reserved scales, each a byte when not all ones,
         * each continued while bit 7 is set. */
        if ((t & 0x03u) != 0x03u) {
            do { if (p >= end) return -1; } while ((*p++ & 0x80u) != 0);
        }
        if ((t & 0x1Cu) != 0x1Cu) {
            do { if (p >= end) return -1; } while ((*p++ & 0x80u) != 0);
        }
        if ((t & 0xE0u) != 0xE0u) {
            do { if (p >= end) return -1; } while ((*p++ & 0x80u) != 0);
        }
    }
    if ((features & 0x08u) == 0)
        return 0;                    /* no I/O: a memory-mapped entry */
    {
        uint8_t io;

        if (p >= end)
            return -1;
        io = *p++;
        into->index = index;
        into->io_lines = (uint8_t)(io & 0x1Fu);
        into->io_base = 0;
        into->io_len = 0;
        if ((io & 0x80u) != 0) {     /* ranges follow */
            uint8_t rd;
            uint32_t base;
            uint32_t len;

            if (p >= end)
                return -1;
            rd = *p++;
            base = field(p, (unsigned)(rd >> 4) & 3u, end, &ok);
            p += k_widths[(rd >> 4) & 3u];
            len = field(p, (unsigned)(rd >> 6) & 3u, end, &ok) + 1u;
            if (!ok || base > 0xFFFFu || len > 0x10000u)
                return -1;
            into->io_base = (uint16_t)base;
            into->io_len = (uint16_t)len;
        }
    }
    return 1;
}

int pcc_cis_parse(const uint8_t *cis, size_t n, pcc_cis *out)
{
    const uint8_t *p = cis;
    const uint8_t *end;
    int have_config = 0;
    int have_any = 0;
    int have_fixed = 0;
    pcc_cis fixed;
    unsigned hops;

    if (cis == (const uint8_t *)0 || out == (pcc_cis *)0)
        return -1;
    end = cis + n;
    out->funcid = 0xFFu;
    out->config_base = 0;
    fixed.funcid = 0xFFu;
    fixed.config_base = 0;
    fixed.index = 0;
    fixed.io_lines = 0;
    fixed.io_base = 0;
    fixed.io_len = 0;

    for (hops = 0; hops < 256u && p < end; hops++) {
        uint8_t code = *p++;
        uint8_t len;
        const uint8_t *body;

        if (code == CISTPL_END)
            break;
        if (code == CISTPL_NULL)
            continue;
        if (p >= end)
            return -1;
        len = *p++;
        body = p;
        if (len == 0xFFu)
            break;                   /* a link to nothing: the end */
        if (body + len > end)
            return -1;
        p += len;

        if (code == CISTPL_FUNCID && len >= 1u) {
            out->funcid = body[0];
        } else if (code == CISTPL_CONFIG && len >= 3u) {
            /* TPCC_SZ: bits 0-1 the width of TPCC_RADR, less one. */
            unsigned rasz = (body[0] & 3u) + 1u;
            uint32_t radr = 0;
            unsigned i;

            if (2u + rasz > len)
                return -1;
            for (i = 0; i < rasz; i++)
                radr |= (uint32_t)body[2u + i] << (8u * i);
            out->config_base = radr;
            have_config = 1;
        } else if (code == CISTPL_CFTABLE && !have_any) {
            pcc_cis e;
            int rc = cftable(body, body + len, &e);

            if (rc < 0)
                return -1;
            if (rc == 1 && e.io_len == 0 && e.io_lines >= 4u) {
                out->index = e.index;
                out->io_lines = e.io_lines;
                out->io_base = 0;
                out->io_len = 0;
                have_any = 1;
            } else if (rc == 1 && e.io_len != 0 && !have_fixed) {
                fixed = e;
                have_fixed = 1;
            }
        }
    }
    if (!have_config)
        return -1;
    if (!have_any) {
        if (!have_fixed)
            return -1;
        out->index = fixed.index;
        out->io_lines = fixed.io_lines;
        out->io_base = fixed.io_base;
        out->io_len = fixed.io_len;
    }
    return 0;
}

int pcc_mem_window(uint32_t host, uint32_t size, uint32_t card, int attr,
                   uint8_t regs[6])
{
    uint32_t stop;
    uint32_t off;

    if (size == 0 || (host & 0xFFFu) != 0 || (card & 0xFFFu) != 0 ||
        host + size - 1u >= 0x1000000u || host + size - 1u < host)
        return -1;
    stop = host + size - 1u;
    /* The offset is card minus host in 4 KB units, modulo the 14 bits the
     * registers hold: the bridge adds it to the host address. */
    off = ((card - host) >> 12) & 0x3FFFu;
    regs[0] = (uint8_t)((host >> 12) & 0xFFu);
    regs[1] = (uint8_t)((host >> 20) & 0x0Fu);
    regs[2] = (uint8_t)((stop >> 12) & 0xFFu);
    regs[3] = (uint8_t)((stop >> 20) & 0x0Fu);
    regs[4] = (uint8_t)(off & 0xFFu);
    regs[5] = (uint8_t)(((off >> 8) & 0x3Fu) | (attr ? 0x40u : 0u));
    return 0;
}

int pcc_io_window(uint16_t base, uint16_t len, uint8_t regs[4])
{
    uint32_t stop;

    if (len == 0 || (uint32_t)base + len - 1u > 0xFFFFu)
        return -1;
    stop = (uint32_t)base + len - 1u;
    regs[0] = (uint8_t)(base & 0xFFu);
    regs[1] = (uint8_t)(base >> 8);
    regs[2] = (uint8_t)(stop & 0xFFu);
    regs[3] = (uint8_t)(stop >> 8);
    return 0;
}

int pcc_io_free(const uint8_t *ports)
{
    unsigned i;

    if (ports == (const uint8_t *)0)
        return 0;
    for (i = 0; i < PCC_IO_PORTS; i++)
        if (ports[i] != 0xFFu)
            return 0;
    return 1;
}

/* Slot `i`'s claim in slots: 0 when no ROM starts there. */
static uint32_t rom_slots(const uint8_t *hdr, uint32_t i)
{
    const uint8_t *h = hdr + 3u * i;
    uint32_t bytes;

    if (h[0] != 0x55u || h[1] != 0xAAu)
        return 0;
    bytes = (uint32_t)h[2] * 512u;
    if (bytes == 0)
        return 1;
    return (bytes + PCC_ROM_SLOT - 1u) / PCC_ROM_SLOT;
}

int32_t pcc_attr_pick(const uint8_t *hdr, uint32_t from)
{
    uint32_t a;

    if (hdr == (const uint8_t *)0 || from >= PCC_ROM_END)
        return -1;
    if (from < PCC_WIN_FIRST)
        from = PCC_WIN_FIRST;
    a = (from + PCC_WIN_SIZE - 1u) & ~(PCC_WIN_SIZE - 1u);
    for (; a + PCC_WIN_SIZE <= PCC_ROM_END; a += PCC_WIN_SIZE) {
        uint32_t first = (a - PCC_ROM_BASE) / PCC_ROM_SLOT;
        uint32_t last = first + PCC_WIN_SIZE / PCC_ROM_SLOT;   /* exclusive */
        uint32_t i;
        int taken = 0;

        /* Any ROM starting at or before the page's last slot whose claim
         * reaches into the page. */
        for (i = 0; i < last && !taken; i++) {
            uint32_t n = rom_slots(hdr, i);

            if (n != 0 && i + n > first)
                taken = 1;
        }
        if (!taken)
            return (int32_t)a;
    }
    return -1;
}

uint32_t pcc_bar_pick(const pcc_range *used, unsigned n, uint64_t ram_top)
{
    uint64_t lo = PCC_BAR_FIRST;
    uint32_t a;

    if (n != 0 && used == (const pcc_range *)0)
        return 0;
    if (ram_top > lo)
        lo = (ram_top + PCC_BAR_SIZE - 1u) & ~(uint64_t)(PCC_BAR_SIZE - 1u);
    if (lo + PCC_BAR_SIZE > PCC_BAR_END)
        return 0;
    a = PCC_BAR_END - PCC_BAR_SIZE;
    for (;;) {
        /* The next candidate is the page under the lowest-starting range
         * that touches `a`. */
        uint32_t below = a;
        int taken = 0;
        unsigned i;

        for (i = 0; i < n; i++) {
            uint64_t b = used[i].base;
            uint64_t e = b + used[i].len;
            uint32_t page;

            if (used[i].len == 0 || b >= (uint64_t)a + PCC_BAR_SIZE || e <= a)
                continue;
            taken = 1;
            page = used[i].base & ~(PCC_BAR_SIZE - 1u);
            if (page < below)
                below = page;
        }
        if (!taken)
            return a;
        if (below < lo + PCC_BAR_SIZE)
            return 0;
        a = below - PCC_BAR_SIZE;
    }
}
