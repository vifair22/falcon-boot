/* falcon-hook's host test binary: the hook's decisions (hookcore.c), which
 * are everything it does apart from the system calls. The system calls are
 * exercised by booting it, which is the coverage harness's job
 * (tools/coverage/). */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = run_hookcore_tests();

    printf("\nfalcon-hook: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
