/* falcon-efi's host test binary: the parts of the loader that are EFI-free by
 * construction, or reach the platform through one pointer a test can fake.
 *
 * The blessed-boot record cache and the Windows preparation state
 * (falcon-efi/src/bless/blesscore.c): manifest bytes in, a label and a boot
 * decision out. The virtual-media override arithmetic
 * (falcon-efi/src/iso/overlay.h). The Secure Boot state reader
 * (falcon-efi/src/secboot/sb.c): its whole input is one runtime services
 * function pointer, so a fake GetVariable reaches every state it can report.
 */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = 0;

    failed += run_bless_tests();
    failed += run_next_tests();
    failed += run_iso_overlay_tests();
    failed += run_sb_tests();

    printf("\nfalcon-efi: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
