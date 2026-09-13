/* Entry point for the host-side unit tests.
 *
 * Everything the loader can exercise off the firmware runs from this one
 * binary, so a single gcov pass covers every unit under test at once.
 *
 * exFAT driver: synthetic in-memory volumes (which reach the error paths a
 * real mkfs.exfat image can never produce), the extent map that the ISO
 * injection framework depends on, and the mkfs.exfat-produced fixture image
 * (which proves the driver agrees with a real implementation).
 *
 * cpio writer and family registry: both are EFI-free by construction, so they
 * are unit-testable here with no firmware and no image at all.
 */

#include "tests.h"

#include <stdio.h>

void ucs2_to_ascii(const uint16_t *src, char *dst, size_t cap)
{
    size_t i = 0;

    if (cap == 0)
        return;
    while (src[i] != 0 && i + 1 < cap) {
        dst[i] = (src[i] >= 0x20u && src[i] < 0x7Fu) ? (char)src[i] : '?';
        i++;
    }
    dst[i] = '\0';
}

int main(void)
{
    int failed = 0;

    failed += run_synth_tests();
    failed += run_file_tests();
    failed += run_extent_tests();
    failed += run_fixture_tests();
    failed += run_cpio_tests();
    failed += run_family_tests();

    printf("\ntotal failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
