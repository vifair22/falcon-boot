/* falcon-bios: taking a block off the top of conventional memory. See
 * lowmem.h. */

#include <mem/lowmem.h>

uint16_t lowmem_segment_for(uint16_t have_kb, uint32_t bytes)
{
    uint32_t want = bytes / 1024u;

    /* A machine reporting less than 256 KB is one whose BDA we do not
     * believe, and taking memory from it would be worse than refusing. */
    if (bytes == 0 || bytes % 1024u != 0 || have_kb < 256u || have_kb <= want)
        return 0;
    if ((uint32_t)(have_kb - want) * 1024u < LOWMEM_FLOOR)
        return 0;
    return (uint16_t)((uint32_t)(have_kb - want) * 1024u / 16u);
}
