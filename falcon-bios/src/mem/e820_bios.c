/* falcon-bios: the BIOS half of e820.h, apart from the arithmetic so the
 * host tests can link that alone: the calls that read the map, and the copy
 * every consumer reads.
 *
 * E820 IS ASKED IN A LOOP AND THE BIOS DECIDES WHEN IT ENDS. EBX carries a
 * continuation value that is opaque, starts at zero, and is zero again on
 * the last entry - though not every BIOS does that, so carry ends it too.
 * The entry is written to ES:DI, which is why the buffer is in the low 64 KB
 * and not a static here: this file links into stage 3, above a megabyte.
 *
 * A BIOS without E820 (before 1998 or so: the machines falcon jump is for,
 * DESIGN Section 13) answers the older questions instead, AX=E801 and
 * AH=88h, and the BDA's word at 0x413 counts conventional memory on every
 * machine. The map is built from those (e820_synth), in the order the
 * kernel's own real-mode setup asks them.
 */

#include <start/bios.h>
#include <mem/e820.h>
#include <mem/lowmem.h>

#include <lib/string.h>

#define E820_SMAP 0x534D4150u   /* "SMAP", which the BIOS echoes in EAX */

static e820_entry g_map[E820_MAX];
static unsigned   g_count;
static int        g_read;

/* One INT 15h call. Nonzero when it returned with carry clear; the
 * registers are in thunk_regs. */
static int int15(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx,
                 uint32_t edi)
{
    thunk_regs.eax = eax;
    thunk_regs.ebx = ebx;
    thunk_regs.ecx = ecx;
    thunk_regs.edx = edx;
    thunk_regs.edi = edi;
    thunk_regs.es = 0;
    thunk_vector = 0x15;
    thunk_bios();
    return (thunk_regs.flags & BIOS_FLAG_CARRY) == 0;
}

static void read_e820(void)
{
    volatile e820_entry *slot = (volatile e820_entry *)(uintptr_t)E820_ADDR;
    uint32_t next = 0;

    do {
        /* The length is asked for as 24 so a BIOS that has the ACPI
         * attributes word reports it; one that does not writes 20 and
         * leaves the rest alone, which is why the slot is cleared each
         * time rather than once. */
        memset((void *)(uintptr_t)slot, 0, sizeof *slot);
        slot->attrs = 1;        /* the value to assume when none is written */

        if (!int15(0xE820u, next, 24, E820_SMAP, (uint32_t)E820_ADDR))
            break;
        if (thunk_regs.eax != E820_SMAP)
            break;
        /* A zero-length entry is not an entry. Some BIOSes emit one rather
         * than ending the loop. */
        if (slot->len != 0 && g_count < E820_MAX) {
            g_map[g_count] = *slot;
            g_count++;
        }
        next = thunk_regs.ebx;
    } while (next != 0 && g_count < E820_MAX);
}

static void read_legacy(void)
{
    e820_legacy in;
    e820_from from = E820_FROM_NONE;

    memset(&in, 0, sizeof in);
    in.base_kb = peek16(BDA_MEMSIZE);
    in.e801 = int15(0xE801u, 0, 0, 0, 0);
    in.e801_ax = (uint16_t)(thunk_regs.eax & 0xFFFFu);
    in.e801_bx = (uint16_t)(thunk_regs.ebx & 0xFFFFu);
    in.e801_cx = (uint16_t)(thunk_regs.ecx & 0xFFFFu);
    in.e801_dx = (uint16_t)(thunk_regs.edx & 0xFFFFu);
    in.ah88 = int15(0x8800u, 0, 0, 0, 0);
    in.ah88_ax = (uint16_t)(thunk_regs.eax & 0xFFFFu);

    g_count = e820_synth(&in, g_map, E820_MAX, &from);
    switch (from) {
    case E820_FROM_E801:
        bios_puts("falcon-bios: no E820; memory map from E801\n");
        break;
    case E820_FROM_88:
        bios_puts("falcon-bios: no E820; memory map from AH=88h\n");
        break;
    default:
        bios_puts("falcon-bios: no E820, E801 or AH=88h; memory map from "
                  "the BDA's count alone\n");
        break;
    }
}

unsigned e820_read(void)
{
    if (g_read)
        return g_count;
    g_read = 1;
    g_count = 0;

    read_e820();
    /* Nothing from E820 is a BIOS that has not got it: the first call came
     * back with carry set, or without "SMAP" in EAX. */
    if (g_count == 0)
        read_legacy();
    return g_count;
}

unsigned e820_count(void)
{
    return g_count;
}

const e820_entry *e820_at(unsigned i)
{
    if (i >= g_count)
        return (const e820_entry *)0;
    return &g_map[i];
}

int e820_is_ram(uint64_t base, uint64_t len)
{
    return e820_is_ram_in(g_map, g_count, base, len);
}

uint64_t e820_place_high(uint64_t size, uint64_t align, uint64_t low,
                         uint64_t high)
{
    return e820_place_in(g_map, g_count, size, align, low, high);
}

uint64_t e820_largest(uint64_t low, uint64_t high)
{
    return e820_largest_in(g_map, g_count, low, high);
}
