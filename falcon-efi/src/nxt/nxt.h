/* falcon-boot: the stationary build's routes out to the machine's other
 * systems through the firmware's own boot entries (docs/DESIGN.md Section
 * 12.4, route NXT).
 *
 * A row is a Boot#### entry the firmware's own boot menu would offer, other
 * than the one this boot came through. Choosing it sets BootNext and resets:
 * the firmware then starts that entry as though falcon had not run, which is
 * the point for a Windows whose BitLocker is sealed to PCR 7. Nothing here
 * reads what an entry boots.
 */

#ifndef FALCON_NXT_H
#define FALCON_NXT_H

#include <efi/uefi.h>
#include <nxt/nxtcore.h>

/* Rows past this are not shown. A machine with more OS boot entries than
 * this is one whose NVRAM wants cleaning, not a longer menu. */
#define NXT_MAX 16u

typedef struct {
    UINT16     option;                 /* the #### of Boot#### */
    CHAR16     label[NXT_LABEL_MAX];   /* its description, or "Boot####" */
    nxt_target target;                 /* the partition and file it names */
} nxt_row;

/* The description of the station's own firmware entry (docs/notes/
 * station-rework.md section 10), which is never a row: choosing it would
 * reboot into this screen. */
#define NXT_OWN_LABEL L"falcon"

/* Reads BootOrder and each entry it names. Returns how many rows it wrote,
 * in BootOrder's order. Zero when BootOrder is missing or names nothing the
 * firmware would offer. Neither the entry this boot came through
 * (BootCurrent) nor one described as NXT_OWN_LABEL is a row. */
UINTN nxt_scan(EFI_SYSTEM_TABLE *st, nxt_row *rows, UINTN cap);

/* Sets BootNext to the row's option and resets the machine. Returns only on
 * failure, with the status of the step that failed and *stage naming it. */
EFI_STATUS nxt_boot(EFI_SYSTEM_TABLE *st, const nxt_row *row,
                    const CHAR16 **stage);

#endif /* FALCON_NXT_H */
