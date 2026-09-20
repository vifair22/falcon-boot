/* falcon-cli's host test binary: the bless tool's logic, split out of the
 * code that spawns helpers and loop-mounts images so it can run here.
 *
 * The ISO view (falcon-cli/src/sign/isoprobe.c): the union of the ISO9660 and
 * UDF listings and the case folding over both, built from listing text, plus
 * one real image for the plumbing. Payload recognition and the PE certificate
 * table (payload.c), the UKI builder (uki.c) and the initrd and cmdline
 * helpers of util.c. Plus bioscore.c, which is where a stick's unallocated
 * sectors are measured before anything is written to them.
 */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = 0;

    failed += run_bioscore_tests();
    failed += run_isoprobe_tests();
    failed += run_payload_tests();
    failed += run_uki_tests();
    failed += run_util_tests();

    printf("\nfalcon-cli: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
