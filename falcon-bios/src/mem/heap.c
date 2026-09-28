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
 * Above 64 MB so it is clear of everything this loader places, and below
 * 4 GiB: a 32-bit base is what an override the BIOS moves is described by,
 * and 4 GiB is the end of what the loader itself can write (biosdefs.h,
 * ADDR_END). When the loader ran in long mode the ceiling was its one-
 * gigabyte identity map instead, and a run near 3 GB on a 4 GB machine took
 * the first store past it and triple faulted.
 */

#include <start/bios.h>
#include <mem/heap.h>
#include <mem/e820.h>

#define HEAP_LOW  (64ull * 1024ull * 1024ull)
#define HEAP_HIGH ADDR_END

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

uint64_t heap_largest(void)
{
    return e820_largest(HEAP_LOW, HEAP_HIGH);
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
