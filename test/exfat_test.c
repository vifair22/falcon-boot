/* Entry point for the exFAT driver unit tests.
 *
 * Two groups: synthetic in-memory volumes (which reach every error path) and
 * the mkfs.exfat-produced fixture image (which proves the driver agrees with
 * a real implementation). Both run in one binary so a single gcov pass covers
 * src/exfat/exfat.c end to end.
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
    failed += run_fixture_tests();

    printf("\ntotal failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
