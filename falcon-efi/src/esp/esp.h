/* falcon-boot: images on an ESP, loaded by device path (docs/DESIGN.md
 * Sections 12.4 and 12.6, route ESP).
 *
 * The station's other way into a system: rather than BootNext and a reset
 * (nxt/), load the file off the volume the firmware already mounted, through
 * the shim-verified load path every other launch takes (Section 7.6). That
 * is Windows Boot Manager where a STATION.INI entry asks for it (`method =
 * efi`), or where an ESP carries it and no firmware entry does (a disk moved
 * in from another machine, an NVRAM reset), and memtest86+ off the station's
 * own ESP. The same volumes are where STATION.INI is read from.
 *
 * Every volume here is the firmware's own FAT driver's: nothing in this
 * module parses a filesystem, which is why the station needs no FAT driver
 * of its own to reach a Windows ESP.
 */

#ifndef FALCON_ESP_H
#define FALCON_ESP_H

#include <efi/uefi.h>

#include <discover/disks.h>

/* Windows Boot Manager's path on its ESP. */
#define ESP_BOOTMGFW L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"

/* A volume carrying a file, and where it is. */
typedef struct {
    EFI_HANDLE handle;
    UINT8      has_part;   /* part below names it (disks_part_of) */
    disks_part part;
} esp_vol;

/* The volume this loader was read from: the station's ESP (Section 12.2).
 * NULL when the firmware did not say. */
EFI_HANDLE esp_own(EFI_SYSTEM_TABLE *st, EFI_HANDLE image);

/* Connects the firmware's filesystem drivers to every partition that has
 * none yet, so a second disk's ESP is a volume even on firmware that
 * connects only what it boots from. Call once, after discover_scan. */
void esp_connect_all(EFI_SYSTEM_TABLE *st);

/* Is `path` a file on the volume behind `handle`? */
BOOLEAN esp_has_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle,
                     const CHAR16 *path);

/* Reads the whole of `path` off the volume into buf. EFI_SUCCESS with *len
 * set; EFI_NOT_FOUND when there is no such file; EFI_BUFFER_TOO_SMALL when
 * it is longer than cap (nothing is kept); the firmware's status
 * otherwise. */
EFI_STATUS esp_read_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle,
                         const CHAR16 *path, void *buf, UINTN cap,
                         UINTN *len);

/* Every partition volume carrying `path`, in the firmware's handle order.
 * Returns how many were written, at most cap. */
UINTN esp_find_all(EFI_SYSTEM_TABLE *st, const CHAR16 *path, esp_vol *out,
                   UINTN cap);

/* Why a launch ended, as pos_result says it. */
typedef struct {
    const CHAR16 *stage;
    EFI_STATUS    status;   /* the failing status, or the image's exit */
    const CHAR16 *advice;   /* one actionable line, or NULL */
} esp_result;

/* Loads `path` off the volume behind `handle` by device path and starts it.
 * `handover` is called between a successful load and the start, for the
 * caller's "handing over" line. EFI_SUCCESS when the image ran and returned
 * (result->status its exit status); otherwise the failing status, with
 * result->stage naming the step. */
EFI_STATUS esp_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                    EFI_HANDLE handle, const CHAR16 *path,
                    void (*handover)(void), esp_result *result);

#endif /* FALCON_ESP_H */
