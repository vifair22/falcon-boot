/* falcon-cli's host test binary: the bless tool's logic, split out of the
 * code that spawns helpers and loop-mounts images so it can run here.
 *
 * The ISO view (falcon-cli/src/sign/isoprobe.c): the union of the ISO9660 and
 * UDF listings and the case folding over both, built from listing text, plus
 * one real image for the plumbing. Payload recognition and the PE certificate
 * table (payload.c), the UKI builder (uki.c) and the initrd and cmdline
 * helpers of util.c. Plus mkstick's bioscore.c, which is where the sectors
 * the BIOS stages take are measured before anything is written to them, and
 * records.c, nextcore.c and nextio.c, the identity records and the
 * next-boot marker, and stationcore.c, where `station install` puts a
 * station and what its STATION.INI says, and kdfbenchcore.c, the lists and
 * table of `mkstick kdf-bench`, and disk.c's lookup of a shelf its OS has
 * open. And interactive create: the drives it offers (drives.c) and the
 * command line its answers make (createui.c).
 */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = 0;

    failed += run_bioscore_tests();
    failed += run_createui_tests();
    failed += run_disk_tests();
    failed += run_drives_tests();
    failed += run_isoprobe_tests();
    failed += run_kdfbenchcore_tests();
    failed += run_payload_tests();
    failed += run_records_tests();
    failed += run_nextio_tests();
    failed += run_stationcore_tests();
    failed += run_totpcore_tests();
    failed += run_updatecore_tests();
    failed += run_uki_tests();
    failed += run_util_tests();

    printf("\nfalcon-cli: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
