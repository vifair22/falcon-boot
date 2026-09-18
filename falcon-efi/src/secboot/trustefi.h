/* falcon-boot: the firmware half of boot predictions (DESIGN Section 7.8).
 *
 * Reads, once at startup, what the platform trusts: db and dbx, shim's
 * runtime mirrors MokListRT and MokListXRT, and SbatLevelRT; plus this stick's
 * own MOK certificate off p1, so the banner and the rows can say "MOK" rather
 * than a CN. Everything is copied into pool memory that lives for the rest of
 * the run, and handed back as the spans trust_evaluate takes.
 *
 * READ-ONLY. These are the same variables `mokutil` and `efi-readvar` read
 * from a running OS. Nothing here writes a variable, and the result gates
 * nothing (trust.h).
 *
 * A variable that is absent is an empty span; one that exists and cannot be
 * read also marks the stores incomplete, and incomplete stores never predict
 * UNTRUSTED or UNSIGNED (trust.h). "Nothing we could read trusts this" is not
 * "nothing trusts this".
 */

#ifndef FALCON_SECBOOT_TRUSTEFI_H
#define FALCON_SECBOOT_TRUSTEFI_H

#include <efi/uefi.h>
#include <trust/trust.h>

/* Fills *out. Never fails: what cannot be read is left empty. image is this
 * loader's own handle, whose device is p1. */
void trust_stores_load(EFI_SYSTEM_TABLE *st, EFI_HANDLE image,
                       trust_stores *out);

#endif /* FALCON_SECBOOT_TRUSTEFI_H */
