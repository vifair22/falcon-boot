/* falcon-bios: the memory map, as the BIOS describes it.
 *
 * The kernel needs this and cannot get it itself: by the time it runs, the
 * BIOS call that answers is gone, because the machine is in protected mode
 * and real mode is where INT 15h lives. Every BIOS loader therefore reads the
 * map and hands it over in the zero page, and Linux has taken it that way
 * since 2.4.
 *
 * The loader wants it too, to place the initrd: it is the only object here
 * whose size is not known until it is read, and the only way to know whether
 * the address chosen for it is memory at all.
 *
 * This file is the arithmetic over a map, host-tested: the map a BIOS
 * without E820 describes with the older calls (e820_synth), where an initrd
 * goes, what a resident carves out, the ceilings a resident answers E801
 * with. The calls themselves, and the copy every consumer reads, are in
 * e820_bios.c.
 */

#include <mem/e820.h>

uint64_t e820_place_in(const e820_entry *map, unsigned n, uint64_t size,
                       uint64_t align, uint64_t low, uint64_t high)
{
    uint64_t best = 0;
    unsigned i;

    if (map == (const e820_entry *)0 || size == 0 || align == 0)
        return 0;

    for (i = 0; i < n; i++) {
        uint64_t start;
        uint64_t end;
        uint64_t at;

        if (map[i].type != E820_USABLE)
            continue;

        start = map[i].base;
        end = map[i].base + map[i].len;   /* exclusive */
        if (start < low)
            start = low;
        if (end > high)
            end = high;
        if (end <= start || end - start < size)
            continue;

        /* As high in the region as the alignment allows, because the low
         * end of memory is where everything else already is. */
        at = (end - size) & ~(align - 1u);
        if (at < start)
            continue;
        if (at > best)
            best = at;
    }
    return best;
}

uint64_t e820_largest_in(const e820_entry *map, unsigned n, uint64_t low,
                         uint64_t high)
{
    uint64_t best = 0;
    unsigned i;

    if (map == (const e820_entry *)0)
        return 0;
    for (i = 0; i < n; i++) {
        uint64_t start = map[i].base;
        uint64_t end = map[i].base + map[i].len;

        if (map[i].type != E820_USABLE)
            continue;
        if (end < start)
            end = UINT64_MAX;
        if (start < low)
            start = low;
        if (end > high)
            end = high;
        if (end > start && end - start > best)
            best = end - start;
    }
    return best;
}

static int carve_emit(e820_entry *out, unsigned cap, unsigned *k,
                      uint64_t base, uint64_t end, uint32_t type,
                      uint32_t attrs)
{
    if (end <= base)
        return 0;
    if (*k >= cap)
        return -1;
    out[*k].base = base;
    out[*k].len = end - base;
    out[*k].type = type;
    out[*k].attrs = attrs;
    (*k)++;
    return 0;
}

int e820_carve(const e820_entry *in, unsigned n, const e820_range *res,
               unsigned n_res, e820_entry *out, unsigned cap,
               unsigned *n_out)
{
    e820_range r[E820_CARVE_MAX];
    unsigned k = 0;
    unsigned i;
    unsigned j;

    if (n_out == (unsigned *)0)
        return -1;
    *n_out = 0;
    if ((in == (const e820_entry *)0 && n != 0) ||
        (res == (const e820_range *)0 && n_res != 0) ||
        n_res > E820_CARVE_MAX || out == (e820_entry *)0)
        return -1;

    /* In address order, so one pass along a usable entry meets them in the
     * order they cut it. Insertion sort: there are at most four. */
    for (i = 0; i < n_res; i++) {
        e820_range v = res[i];

        for (j = i; j > 0 && r[j - 1u].base > v.base; j--)
            r[j] = r[j - 1u];
        r[j] = v;
    }

    for (i = 0; i < n; i++) {
        uint64_t cur = in[i].base;
        uint64_t end;

        /* An entry that runs past the top of the address space ends there. */
        end = (in[i].len > UINT64_MAX - cur) ? UINT64_MAX : cur + in[i].len;

        if (in[i].type != E820_USABLE) {
            if (k >= cap)
                return -1;
            out[k++] = in[i];
            continue;
        }
        for (j = 0; j < n_res; j++) {
            uint64_t rs = r[j].base;
            uint64_t re = (r[j].len > UINT64_MAX - rs) ? UINT64_MAX
                                                       : rs + r[j].len;

            if (rs < cur)
                rs = cur;
            if (re > end)
                re = end;
            if (re <= rs)
                continue;
            if (carve_emit(out, cap, &k, cur, rs, E820_USABLE,
                           in[i].attrs) != 0 ||
                carve_emit(out, cap, &k, rs, re, E820_RESERVED,
                           in[i].attrs) != 0)
                return -1;
            cur = re;
        }
        if (carve_emit(out, cap, &k, cur, end, E820_USABLE,
                       in[i].attrs) != 0)
            return -1;
    }
    *n_out = k;
    return 0;
}

uint16_t e820_e801_cap(const e820_range *res, unsigned n_res)
{
    const uint64_t floor = 16ull * 1024ull * 1024ull;
    uint64_t lowest = UINT64_MAX;
    unsigned i;

    if (res == (const e820_range *)0)
        return 0xFFFFu;
    for (i = 0; i < n_res; i++) {
        uint64_t start = res[i].base;
        uint64_t end;

        if (res[i].len == 0)
            continue;
        end = (res[i].len > UINT64_MAX - start) ? UINT64_MAX
                                                : start + res[i].len;
        if (end <= floor)
            continue;
        if (start < floor)
            start = floor;
        if (start < lowest)
            lowest = start;
    }
    if (lowest == UINT64_MAX || ((lowest - floor) >> 16) >= 0xFFFFu)
        return 0xFFFFu;
    return (uint16_t)((lowest - floor) >> 16);
}

int e820_is_ram_in(const e820_entry *map, unsigned n, uint64_t base,
                   uint64_t len)
{
    uint64_t end;
    uint64_t at;
    unsigned i;
    unsigned pass;

    if (map == (const e820_entry *)0 || len == 0 || base + len < base)
        return 0;
    end = base + len;

    for (i = 0; i < n; i++) {
        if (map[i].type != E820_USABLE && map[i].len != 0 &&
            map[i].base < end && base < map[i].base + map[i].len)
            return 0;
    }
    /* Walk the range through the usable entries that cover it, in any order
     * the map lists them; each pass either advances or the range has a hole.
     * n passes is enough for every entry to be used once. */
    at = base;
    for (pass = 0; pass <= n && at < end; pass++) {
        uint64_t next = at;

        for (i = 0; i < n; i++) {
            uint64_t e_end = map[i].base + map[i].len;

            if (map[i].type == E820_USABLE && map[i].base <= at &&
                e_end > next)
                next = e_end;
        }
        if (next == at)
            return 0;
        at = next;
    }
    return at >= end;
}

uint16_t e820_e801_cap_low(const e820_range *res, unsigned n_res)
{
    const uint64_t one = 1024ull * 1024ull;
    const uint64_t sixteen = 16ull * 1024ull * 1024ull;
    uint64_t lowest = sixteen;
    unsigned i;

    if (res == (const e820_range *)0)
        return 0xFFFFu;
    for (i = 0; i < n_res; i++) {
        uint64_t start = res[i].base;
        uint64_t end;

        if (res[i].len == 0)
            continue;
        end = (res[i].len > UINT64_MAX - start) ? UINT64_MAX
                                                : start + res[i].len;
        if (end <= one || start >= sixteen)
            continue;
        if (start < one)
            start = one;
        if (start < lowest)
            lowest = start;
    }
    if (lowest >= sixteen)
        return 0xFFFFu;
    return (uint16_t)((lowest - one) >> 10);
}

/* The 1-16 MB and above-16 MB halves of AX=E801's answer, or the AX=0 form
 * where the counts are in CX and DX. 0 when the answer is not one: a BIOS
 * that has no E801 and left AX alone leaves 0xE801 in it, which is more
 * than the 15 MB the low half can mean. */
static int synth_e801(const e820_legacy *in, e820_entry *out, unsigned cap,
                      unsigned *k)
{
    const uint64_t one = 1024ull * 1024ull;
    const uint64_t sixteen = 16ull * 1024ull * 1024ull;
    uint32_t kb = in->e801_ax != 0 ? in->e801_ax : in->e801_cx;
    uint32_t blocks = in->e801_bx != 0 ? in->e801_bx : in->e801_dx;
    uint64_t end;

    if (kb > 0x3C00u)
        return 0;
    end = one + (uint64_t)kb * 1024ull;
    /* Below 16 MB the count is contiguous from 1 MB, so what it stops short
     * of is a hole (the ISA hole at 15 MB, or memory the machine does not
     * have): reserved, so nothing is placed in it. */
    if (carve_emit(out, cap, k, one, end, E820_USABLE, 1u) != 0 ||
        carve_emit(out, cap, k, end, sixteen, E820_RESERVED, 1u) != 0)
        return 1;
    carve_emit(out, cap, k, sixteen, sixteen + (uint64_t)blocks * 65536ull,
               E820_USABLE, 1u);
    return 1;
}

unsigned e820_synth(const e820_legacy *in, e820_entry *out, unsigned cap,
                    e820_from *from)
{
    const uint64_t one = 1024ull * 1024ull;
    const uint64_t video = 0xA0000ull;
    e820_from src = E820_FROM_BDA;
    unsigned k = 0;
    uint64_t low;

    if (from != (e820_from *)0)
        *from = E820_FROM_NONE;
    if (in == (const e820_legacy *)0 || out == (e820_entry *)0)
        return 0;

    /* Conventional memory as the BDA counts it; what the BIOS keeps above
     * its count (the EBDA), then the video and ROM area up to 1 MB. */
    low = (uint64_t)in->base_kb * 1024ull;
    if (low > video)
        low = video;
    if (carve_emit(out, cap, &k, 0, low, E820_USABLE, 1u) != 0 ||
        carve_emit(out, cap, &k, low, video, E820_RESERVED, 1u) != 0 ||
        carve_emit(out, cap, &k, video, one, E820_RESERVED, 1u) != 0)
        return k;

    if (in->e801 && synth_e801(in, out, cap, &k)) {
        src = E820_FROM_E801;
    } else if (in->ah88) {
        carve_emit(out, cap, &k, one, one + (uint64_t)in->ah88_ax * 1024ull,
                   E820_USABLE, 1u);
        src = E820_FROM_88;
    }
    if (from != (e820_from *)0)
        *from = src;
    return k;
}
