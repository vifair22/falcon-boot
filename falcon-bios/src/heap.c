/* falcon-bios: memory above the loader (docs/notes/bios-route.md Section
 * 4.3).
 *
 * Everything else in this build places its buffers as statics, because every
 * other route knows how big they are: a command line, a setup header, a
 * handful of CD sectors. Windows does not. The tail its patched boot.wim
 * appends is sized from the WIM's own metadata and lookup table, which are
 * read off the medium and can be tens of megabytes, so the one thing that
 * cannot be written down in advance is how much room it needs.
 *
 * Hence a bump allocator over a run of memory taken from the E820 map. There
 * is no free, and that is not a shortcut: one of these boots happens at a
 * time and ends by handing the machine to somebody else.
 *
 * Above 64 MB so it is clear of everything this loader places, and below the
 * end of the identity map, which is the real ceiling: a 32-bit base is what
 * an override the BIOS moves is described by, but this loader writes the
 * buffer itself first, and it can only write what its own page tables
 * describe. Taking the highest run under 4 GB on a 4 GB machine put it near
 * 3 GB, outside the gigabyte lmode.S maps, and the first store triple
 * faulted.
 */

#include <bios.h>
#include <heap.h>
#include <e820.h>

#define HEAP_LOW  (64ull * 1024ull * 1024ull)
#define HEAP_HIGH ((uint64_t)IDENTITY_MAP)

static uint64_t g_base;
static uint64_t g_len;
static uint64_t g_used;

int heap_open(uint64_t want)
{
    uint64_t at;

    g_base = 0;
    g_len = 0;
    g_used = 0;
    if (want == 0)
        return 0;

    at = e820_place_high(want, 4096u, HEAP_LOW, HEAP_HIGH);
    if (at == 0)
        return 0;
    g_base = at;
    g_len = want;
    return 1;
}

void *heap_alloc(uint64_t n)
{
    uint64_t at;

    if (g_len == 0 || n == 0)
        return (void *)0;
    n = (n + 4095u) & ~(uint64_t)4095u;
    if (n > g_len - g_used)
        return (void *)0;
    at = g_base + g_used;
    g_used += n;
    return (void *)(uintptr_t)at;
}

uint32_t heap_phys(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

void heap_reset(void)
{
    g_used = 0;
}
