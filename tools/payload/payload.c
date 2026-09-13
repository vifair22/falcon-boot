/* Test payload for the ISO boot path: the EFI application that
 * tools/mk-test-iso.sh puts at \EFI\BOOT\BOOTX64.EFI inside the El Torito
 * image. It exists to prove the chainload end to end, so it does the three
 * things the assertion needs and nothing else: print a marker the rig can
 * grep for on the serial console, stay up long enough to be visible, and
 * return so the loader's teardown path gets exercised too.
 *
 * Deliberately outside src/: this never ships in the loader.
 */

#include <efi/uefi.h>

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    (void)image_handle;

    st->ConOut->OutputString(st->ConOut, L"\r\nFALCON-PAYLOAD-OK\r\n");
    st->BootServices->Stall(2000000);
    st->ConOut->OutputString(st->ConOut, L"FALCON-PAYLOAD-EXIT\r\n");
    return EFI_SUCCESS;
}
