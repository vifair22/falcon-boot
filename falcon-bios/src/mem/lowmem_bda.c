/* falcon-bios: the BDA half of lowmem.h, apart from the arithmetic so the
 * host tests can link that alone. */

#include <start/bios.h>
#include <mem/lowmem.h>

uint16_t lowmem_segment(uint32_t bytes)
{
    return lowmem_segment_for(peek16(BDA_MEMSIZE), bytes);
}

void lowmem_take(uint16_t seg)
{
    poke16(BDA_MEMSIZE, (uint16_t)((uint32_t)seg * 16u / 1024u));
}
