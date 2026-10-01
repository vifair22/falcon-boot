/* falcon-appliance's host test binary. The appliance is PID 1 in an initramfs
 * and cannot be stood in for by a host process; route.c, what each file on p2
 * is owed (DESIGN Sections 7.2 and 7.5), is split out so it can be: three
 * enums in and one out. stickid.c, which partitions are the stick's, is
 * split out the same way.
 */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = run_appliance_route_tests();

    failed += run_appliance_stickid_tests();

    printf("\nfalcon-appliance: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
