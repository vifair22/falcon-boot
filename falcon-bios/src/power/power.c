/* falcon-bios: powering the machine off. See power.h. */

#include <power/power.h>

#include <start/bios.h>
#include <hw/modrt.h>
#include <lib/string.h>
#include <power/acpicore.h>

#define APM_SIG 0x504Du   /* "PM" in BX from the installation check */

static int apm_call(uint32_t ax, uint32_t bx, uint32_t cx)
{
    thunk_regs.eax = ax;
    thunk_regs.ebx = bx;
    thunk_regs.ecx = cx;
    thunk_vector = 0x15;
    thunk_bios();
    return (thunk_regs.flags & BIOS_FLAG_CARRY) == 0;
}

static void apm_off(void)
{
    if (!apm_call(0x5300u, 0, 0) || (thunk_regs.ebx & 0xFFFFu) != APM_SIG) {
        bios_puts("falcon-bios: power off: no APM\n");
        return;
    }
    /* Connect the real-mode interface (already connected is an error the
     * call may give, and harmless), say driver version 1.2, enable power
     * management for all devices; then all devices off. */
    (void)apm_call(0x5301u, 0, 0);
    (void)apm_call(0x530Eu, 0, 0x0102u);
    (void)apm_call(0x5308u, 1, 1);
    (void)apm_call(0x5307u, 1, 3);
    bios_puts("falcon-bios: power off: APM did not take\n");
}

/* Paging is off and memory is identity-mapped: an address is a pointer. */
static int phys_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr == 0 || addr > 0xFFFFFFFFu - len)
        return -1;
    memcpy(buf, (const void *)(uintptr_t)addr, len);
    return 0;
}

static void acpi_off(void)
{
    acpi_s5 s;
    uint32_t i;
    int rc = acpi_find_s5(phys_read, (void *)0,
                          (uint32_t)peek16(0x40Eu) << 4, &s);

    if (rc != ACPI_OK) {
        bios_puts("falcon-bios: power off: no ACPI S5\n");
        return;
    }
    /* SCI_EN clear: the firmware has not handed ACPI over, and a write to
     * PM1_CNT would go nowhere. The SMI command port does the handover. */
    if ((inw((uint16_t)s.pm1a_cnt) & 1u) == 0 && s.smi_cmd != 0 &&
        s.smi_cmd <= 0xFFFFu && s.acpi_enable != 0) {
        outb((uint16_t)s.smi_cmd, s.acpi_enable);
        for (i = 0; i < 3000u && (inw((uint16_t)s.pm1a_cnt) & 1u) == 0; i++)
            io_delay_us(1000u);
    }
    outw((uint16_t)s.pm1a_cnt, acpi_s5_word(s.slp_typa));
    if (s.pm1b_cnt != 0)
        outw((uint16_t)s.pm1b_cnt, acpi_s5_word(s.slp_typb));
    io_delay_us(100000u);
    bios_puts("falcon-bios: power off: ACPI S5 did not take\n");
}

void power_off(void)
{
    apm_off();
    acpi_off();
}
