/* falcon-core's host test binary.
 *
 * exFAT driver: synthetic in-memory volumes (which reach the error paths a
 * real mkfs.exfat image can never produce), the extent map that the ISO
 * injection framework depends on, and the mkfs.exfat-produced fixture image
 * (which proves the driver agrees with a real implementation).
 *
 * cpio writer, family registry, .osrel reader and the blessed-artifact
 * manifest: EFI-free by construction, so unit-testable here with no firmware
 * and no image at all.
 *
 * crypto and the LUKS2 JSON scanner: published test vectors and embedded
 * snippets, no image either. Hand-written crypto checked only against itself
 * is worth nothing, so every primitive is pinned to FIPS/RFC/IEEE vectors.
 *
 * The filesystem registry (falcon-core/src/fs): the seam between the drivers
 * and their consumers, tested on synthetic volumes plus the ext4 fixture, so
 * that dressing the drivers in one vtable cannot change what a consumer sees.
 *
 * LUKS2 and ext4: fixture images that cryptsetup and mkfs.ext4 themselves
 * produced (falcon-core/tools/mk-test-luks.sh,
 * falcon-core/tools/mk-test-ext4.sh), which is the only evidence that these
 * readers agree with the implementations that wrote the bytes. Both groups skip
 * cleanly when their image is absent.
 *
 * ISO9660 and UDF (falcon-core/src/fs/iso9660.c, udf.c): synthetic images for
 * the structures no tool writes, and fixtures xorriso, mkudffs and the
 * kernel's UDF writer produced, walked through the registry against their
 * manifests.
 *
 * The PE walker and the boot predictions (falcon-core/src/pe,
 * falcon-core/src/trust): synthetic headers, plus certificates and images
 * falcon-core/tools/mk-test-trust.sh built with known relationships.
 */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = 0;

    failed += run_survey_tests();
    failed += run_tui_tests();
    failed += run_shelf_tests();
    failed += run_nextboot_tests();
    failed += run_synth_tests();
    failed += run_file_tests();
    failed += run_extent_tests();
    failed += run_fixture_tests();
    failed += run_cpio_tests();
    failed += run_family_tests();
    failed += run_manifest_tests();
    failed += run_crypto_tests();
    failed += run_json_tests();
    failed += run_luks_tests();
    failed += run_ext4_tests();
    failed += run_iso9660_tests();
    failed += run_udf_tests();
    failed += run_udfedit_tests();
    failed += run_media_fs_tests();
    failed += run_bootcfg_tests();
    failed += run_extmap_tests();
    failed += run_hookcmd_tests();
    failed += run_grubcfg_tests();
    failed += run_bsdmd_tests();
    failed += run_xpsetup_tests();
    failed += run_eltorito_tests();
    failed += run_fat_tests();
    failed += run_medium_tests();
    failed += run_bootfs_tests();
    failed += run_blesslist_tests();
    failed += run_wim_tests();
    failed += run_fs_tests();
    failed += run_part_tests();
    failed += run_table_tests();
    failed += run_ident_tests();
    failed += run_osrel_tests();
    failed += run_pe_tests();
    failed += run_trust_tests();
    failed += run_gz_tests();
    failed += run_rebl_tests();

    printf("\nfalcon-core: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
