/* falcon-boot: lines for the serial port only, the diagnostics a person at
 * the machine cannot act on.
 *
 * The first EFI_SERIAL_IO_PROTOCOL instance, written directly, so nothing
 * reaches the screen. A machine without one logs nothing. Under OVMF it is
 * the port the rig's serial log records.
 */

#ifndef FALCON_EFI_SERIAL_H
#define FALCON_EFI_SERIAL_H

#include <efi/uefi.h>

/* s, ASCII, as one line: CRLF before and after. */
void serial_line(EFI_SYSTEM_TABLE *st, const char *s);

#endif /* FALCON_EFI_SERIAL_H */
