/* falcon-boot: the time-stamp counter (docs/DESIGN.md Sections 5.5 and 9).
 *
 * The clock behind the boot-timing variables (loader/bli.h): the counter
 * itself, its rate, and the time spent waiting on the keyboard. The tcg
 * module's RNG seed reads the counter too. The arithmetic is tsccore.c's.
 */

#ifndef FALCON_EFI_TSC_H
#define FALCON_EFI_TSC_H

#include <efi/uefi.h>

/* RDTSC: ticks since the processor came out of reset. */
UINT64 tsc_read(void);

/* IA32_APERF and IA32_MPERF on the calling processor, for the effective
 * clock (tsccore.h, tsc_eff_mhz). 1 with both read; 0, with neither read,
 * when CPUID leaf 6 does not enumerate them or a hypervisor is present
 * (CPUID.1:ECX bit 31): a guest may take #GP on them, and the firmware's
 * handler for an exception it did not expect halts. */
int tsc_perf_read(UINT64 *aperf, UINT64 *mperf);

/* How the rate was had, for the serial line. */
typedef enum {
    TSC_HZ_NONE  = 0,   /* no sane answer: no timestamps this boot */
    TSC_HZ_CPUID = 1,   /* leaves 0x15 and 0x16 */
    TSC_HZ_STALL = 2    /* counted across gBS->Stall */
} tsc_hz_how;

/* The counter's rate in Hz, 0 when it could not be had. The first call
 * decides and the answer is kept; a calibration costs that call
 * TSC_CAL_USEC of Stall. */
UINT64 tsc_hz(EFI_BOOT_SERVICES *bs, tsc_hz_how *how);

/* A wait on the keyboard starts or ends. Every wait for input the loaders
 * have goes through these: the menus, the passphrase prompt and every
 * "press any key" page (console/). They may nest; the outermost counts. */
void tsc_wait_begin(void);
void tsc_wait_end(void);

/* The ticks spent waiting so far, open wait included. */
UINT64 tsc_wait_ticks(void);

#endif /* FALCON_EFI_TSC_H */
