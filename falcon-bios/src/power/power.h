/* falcon-bios: powering the machine off (docs/DESIGN.md Section 7.9).
 *
 * The idle power-off's BIOS half, in the order a machine of each age
 * answers: APM (INT 15h AX=5307h, "all devices off"), which every BIOS from
 * the A21m's era has and SeaBIOS turns into ACPI S5 itself; then ACPI S5
 * directly (power/acpicore.c finds the values), ACPI switched on through
 * its SMI command port first when the firmware left it off. Each is tried
 * only when the one before it returned. Diagnostics go to serial.
 */

#ifndef FALCON_BIOS_POWER_H
#define FALCON_BIOS_POWER_H

/* Returns only when neither took. */
void power_off(void);

#endif /* FALCON_BIOS_POWER_H */
