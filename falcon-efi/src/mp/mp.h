/* falcon-boot: argon2id's lanes on the machine's other processors, through
 * the firmware's MP services (DESIGN Section 9).
 *
 * Purpose. The UEFI half of the lane runner: falcon-core's lane pool
 * (crypto/lanes.h) with application processors as its workers, started
 * once for an unlock with StartupAllAPs and released when it is done.
 * Each worker borrows its own processor's AVX state (CR4 and XCR0 are per
 * processor) and gives it back before it returns to the firmware.
 *
 * Nothing here is needed for an unlock to work: no MP services, one
 * processor, a firmware that will not dispatch without blocking, and
 * mp_lanes_start hands back NULL, which argon2id takes as lanes in turn.
 *
 * What an application processor may do is narrow: no boot services, no
 * console, the firmware's stack (edk2 gives each 32 KiB; a segment's job
 * uses about 5.5). The procedure runs falcon-core's job, the AVX borrow
 * and WhoAmI, which the specification allows from an AP, and nothing
 * else.
 *
 * Interactions. falcon-efi/src/pos/pos.c starts it around luks_unlock,
 * after borrowing the boot processor's AVX state, whose level the workers
 * must match.
 */

#ifndef FALCON_EFI_MP_H
#define FALCON_EFI_MP_H

#include <efi/uefi.h>
#include <efi/mpservices.h>
#include <crypto/argon2.h>
#include <crypto/lanes.h>
#include <mp/mpcore.h>

typedef struct {
    lanes_pool                pool;
    argon2_runner             runner;
    EFI_MP_SERVICES_PROTOCOL *mp;
    EFI_EVENT                 done;     /* signalled when every AP returned */
    uint8_t                   worker[MP_TABLE_MAX];
    uint32_t                  workers;
    int                       started;
} mp_lanes;

/* Starts the workers for derivations at block function level level. The
 * runner to hand argon2id, or NULL when no other processor runs lanes. */
const argon2_runner *mp_lanes_start(EFI_BOOT_SERVICES *bs, mp_lanes *m,
                                    int level);

/* Releases the workers and waits for the firmware to see them back (edk2
 * notices within 100 ms). Nothing when start returned NULL. */
void mp_lanes_stop(EFI_BOOT_SERVICES *bs, mp_lanes *m);

/* Processors that ran lanes: the boot processor and the workers that
 * joined. */
uint32_t mp_lanes_count(const mp_lanes *m);

#endif /* FALCON_EFI_MP_H */
