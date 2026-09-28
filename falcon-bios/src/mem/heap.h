/* falcon-bios: memory above the loader, for the one route that needs more
 * than a static. See heap.c.
 */

#ifndef FALCON_BIOS_HEAP_H
#define FALCON_BIOS_HEAP_H

#include <stddef.h>
#include <stdint.h>

/* Takes the highest usable run of at least `want` bytes between 64 MB and
 * 4 GiB (ADDR_END), from the E820 map. 0 when there is no such run; the
 * caller says so and does not go on. */
int heap_open(uint64_t want);

/* The most heap_open could take: the largest usable run between 64 MB and
 * 4 GiB. */
uint64_t heap_largest(void);

/* A page-aligned block, or NULL when the run is exhausted. There is no free:
 * one of these boots happens at a time. What of it the handler serves after
 * the handoff is taken out of the map the payload is handed (vcd.c, the
 * appended region), so the payload does not allocate over it. */
void *heap_alloc(uint64_t n);

/* The physical address of a pointer heap_alloc returned, which is what an
 * override the BIOS moves has to be told. Paging is off, so this is the
 * pointer; it is a function because saying so once is better than casting at
 * every call. */
uint32_t heap_phys(const void *p);

/* Forgets every block. Nothing is zeroed: a caller that held a secret wipes
 * it itself, and this route holds none. */
void heap_reset(void);

#endif /* FALCON_BIOS_HEAP_H */
