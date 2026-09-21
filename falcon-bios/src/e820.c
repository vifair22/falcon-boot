/* falcon-bios: the memory map, from INT 15h AX=E820
 * (docs/notes/bios-route.md Section 4.2).
 *
 * The kernel needs this and cannot get it itself: by the time it runs, the
 * BIOS call that answers is gone, because the machine is in long mode and
 * real mode is where INT 15h lives. Every BIOS loader therefore reads the
 * map and hands it over in the zero page, and Linux has taken it that way
 * since 2.4.
 *
 * The loader wants it too, to place the initrd: it is the only object here
 * whose size is not known until it is read, and the only way to know whether
 * the address chosen for it is memory at all.
 *
 * E820 IS ASKED IN A LOOP AND THE BIOS DECIDES WHEN IT ENDS. EBX carries a
 * continuation value that is opaque, starts at zero, and is zero again on
 * the last entry - though not every BIOS does that, so carry ends it too.
 * The entry is written to ES:DI, which is why the buffer is in the low 64 KB
 * and not a static here: this file links into stage 3, above a megabyte.
 */

#include <bios.h>
#include <e820.h>

#include <lib/string.h>

#define E820_SMAP 0x534D4150u   /* "SMAP", which the BIOS echoes in EAX */

static e820_entry g_map[E820_MAX];
static unsigned   g_count;
static int        g_read;

unsigned e820_read(void)
{
    volatile e820_entry *slot = (volatile e820_entry *)(uintptr_t)E820_ADDR;
    uint32_t next = 0;

    if (g_read)
        return g_count;
    g_read = 1;
    g_count = 0;

    do {
        /* The length is asked for as 24 so a BIOS that has the ACPI
         * attributes word reports it; one that does not writes 20 and
         * leaves the rest alone, which is why the slot is cleared each
         * time rather than once. */
        memset((void *)(uintptr_t)slot, 0, sizeof *slot);
        slot->attrs = 1;        /* the value to assume when none is written */

        thunk_regs.eax = 0xE820u;
        thunk_regs.ebx = next;
        thunk_regs.ecx = 24;
        thunk_regs.edx = E820_SMAP;
        thunk_regs.edi = (uint32_t)E820_ADDR;
        thunk_regs.es = 0;
        thunk_vector = 0x15;
        thunk_bios();

        if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
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

/* The cached map, which is the only one the loader has. Split from the line
 * above it so the arithmetic can be handed a map a test wrote. */
uint64_t e820_place_high(uint64_t size, uint64_t align, uint64_t low,
                         uint64_t high)
{
    return e820_place_in(g_map, g_count, size, align, low, high);
}
