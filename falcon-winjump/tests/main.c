/* falcon-winjump's host test binary: the arithmetic of the driver-mount
 * fallback (winjumpcore). The rest of winjump.c is Win32 glue that runs only
 * in a WinPE. */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = 0;

    failed += run_winjumpcore_tests();

    printf("\nfalcon-winjump: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
