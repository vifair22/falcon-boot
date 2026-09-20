/* falcon-bios's host test binary.
 *
 * What is testable here is what Section 5.3 of docs/notes/bios-route.md
 * insists on: the decisions. The assembly is transport and is exercised by
 * booting it under SeaBIOS (falcon-efi/tools/run-qemu.sh --bios), and the
 * rest of the C is a thin layer over BIOS interrupts that only exist on a
 * machine. What is left is the arithmetic the virtual CD's INT 13h handler
 * copies, and that is exactly the part with no analyzer over it, so it is
 * worked out in C and checked here.
 */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = run_vcdcore_tests();

    printf("\nfalcon-bios: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
