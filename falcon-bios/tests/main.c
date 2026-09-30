/* falcon-bios's host test binary.
 *
 * What is testable here is what Section 5.3 of docs/notes/bios-route.md
 * insists on: the decisions. The assembly is transport and is exercised by
 * booting it under SeaBIOS (falcon-efi/tools/run-qemu.sh --bios), and the
 * rest of the C is a thin layer over BIOS interrupts that only exist on a
 * machine. What is left is the arithmetic: the virtual CD's INT 13h handler
 * (vcdcore), the addresses the x86 boot protocol is handed and the padding
 * between concatenated initrds (linuxcore), and the placement that chooses
 * an initrd's address out of the BIOS memory map (e820), falcon jump's
 * JUMP.INI (jumpini), where a misread line would boot the wrong thing, and
 * the partition stage 2 takes stage 3 from (bootpart). Every one of them
 * fails silently on a real machine, and the assembly around them is the part
 * of this repository no analyzer reads.
 */

#include "tests.h"

#include <stdio.h>

int main(void)
{
    int failed = 0;

    failed += run_vcdcore_tests();
    failed += run_vfdcore_tests();
    failed += run_linuxcore_tests();
    failed += run_e820_tests();
    failed += run_jumpini_tests();
    failed += run_jmod_tests();
    failed += run_lowmem_tests();
    failed += run_atacore_tests();
    failed += run_usbcore_tests();
    failed += run_pcccore_tests();
    failed += run_bootpart_tests();
    failed += run_vbrcore_tests();
    failed += run_cdclock_tests();
    failed += run_acpicore_tests();

    printf("\nfalcon-bios: total failed: %d\n", failed);
    return failed == 0 ? 0 : 1;
}
