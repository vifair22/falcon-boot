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
 * cpio writer, family registry, the blessed-artifact manifest and the
 * blessed-boot record cache that consumes it: all four are EFI-free by
 * construction, so they are unit-testable here with no firmware and no image
 * at all.
 *
 * The bless tool's view of an ISO (src/cli/sign/isoprobe.c) is host code
 * rather than loader code, but it is here for the same reason: the union of
 * the ISO9660 and UDF listings, and the case folding over both, are built
 * from listing text, so they need neither a helper process nor a
 * multi-gigabyte image.
 *
 * crypto and the LUKS2 JSON scanner: published test vectors and embedded
 * snippets, no image either. Hand-written crypto checked only against itself
 * is worth nothing, so every primitive is pinned to FIPS/RFC/IEEE vectors.
 *
 * The filesystem registry (src/fs): the seam between the drivers and their
 * consumers, tested on synthetic volumes plus the ext4 fixture, so that
 * dressing the drivers in one vtable cannot change what a consumer sees.
 *
 * The Secure Boot state reader (src/sb): its whole input is one runtime
 * services function pointer, so a fake GetVariable reaches every state it can
 * report, with no firmware and no image either.
 *
 * LUKS2 and ext4: fixture images that cryptsetup and mkfs.ext4 themselves
 * produced (tools/mk-test-luks.sh, tools/mk-test-ext4.sh), which is the only
 * evidence that these readers agree with the implementations that wrote the
 * bytes. Both groups skip cleanly when their image is absent.
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
    failed += run_isoprobe_tests();
    failed += run_payload_tests();
    failed += run_wimext_tests();
    failed += run_prepcore_tests();
    failed += run_appliance_route_tests();
    failed += run_manifest_tests();
    failed += run_bless_tests();
    failed += run_prep_tests();
    failed += run_crypto_tests();
    failed += run_json_tests();
    failed += run_luks_tests();
    failed += run_ext4_tests();
    failed += run_fs_tests();
    failed += run_iso_overlay_tests();
    failed += run_part_tests();
    failed += run_sb_tests();

    printf("\ntotal failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
