/* falcon-boot: which processors take argon2id lanes, the EFI-free half of
 * the UEFI lane runner (mp.c), host-tested.
 *
 * The firmware's MP services start every enabled application processor;
 * each one asks whether it is one of the workers. A worker is the first
 * usable thread of a core the boot processor is not on: two lanes on the
 * threads of one core share its execution units and run little faster
 * than one, and a lane beside the boot processor's would slow the lane it
 * runs.
 * When the firmware reports no topology that tells cores apart (every
 * processor thread 0 of core 0, say), every enabled, healthy application
 * processor is a worker instead. Either way at most MP_WORKERS_MAX.
 *
 * Interactions. mp.c fills the table from GetProcessorInfo and reads the
 * answer per processor from its procedure.
 */

#ifndef FALCON_EFI_MPCORE_H
#define FALCON_EFI_MPCORE_H

#include <stddef.h>
#include <stdint.h>

/* Processors the table describes, and workers admitted, at most. Past the
 * table a machine's processors are simply not asked. */
#define MP_TABLE_MAX   256u
#define MP_WORKERS_MAX 63u

typedef struct {
    uint32_t package;
    uint32_t core;
    uint32_t thread;
    uint8_t  bsp;       /* the boot processor */
    uint8_t  usable;    /* enabled and healthy */
} mp_cpu;

/* Marks worker[i] for each processor of cpus[0..n) that takes lanes, and
 * returns how many do. */
uint32_t mp_pick_workers(const mp_cpu *cpus, size_t n, uint8_t *worker);

#endif /* FALCON_EFI_MPCORE_H */
