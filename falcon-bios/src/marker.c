/* falcon-bios: the next-boot marker, taken off the boot disk (DESIGN Section
 * 6.3.1).
 *
 * The UEFI loader keeps "boot this next" in an EFI variable, which it reads
 * and deletes. A BIOS has no variable store, so the marker is one sector in
 * the gap in front of p1 (biosdefs.h, NEXTBOOT_LBA), written from Linux and
 * read here. falcon-core/src/media/nextboot.h has its format; this file is
 * the half that needs a BIOS: the sector read, the write that clears it, and
 * the clock that dates it.
 *
 * CLEAR FIRST, AS THE UEFI LOADER DELETES FIRST. The marker is zeroed before
 * a byte of it is believed, and every path below - a value that will not
 * parse, one that has gone stale, an entry that is not on the stick, a boot
 * that starts and comes back - therefore leaves nothing on the disk that
 * could make it happen again. Validating first and clearing after would make
 * every failure that could not boot the entry repeat on every boot.
 *
 * What entitles the loader to write the sector is the magic, and nothing
 * else: a sector that does not begin with it is not ours and is left exactly
 * as it was, whatever it holds. And if the write does not take (a stick with
 * a write-protect switch, a BIOS that refuses AH=43h) the marker is not acted
 * on at all, because a marker that cannot be cleared is not a one-shot; the
 * window then retires it, since it goes stale however often it is read.
 *
 * WHY IT IS DATED. A reboot from Linux follows the machine's boot order, and
 * on a BIOS nothing can make it come back to the stick. A stick picked from a
 * boot menu is not in that order, so the marker would wait until the stick
 * was next booted, days later perhaps, and then start something nobody asked
 * for. So it carries the RTC's reading when it was set and is honoured only
 * within NEXTBOOT_WINDOW_SECONDS of it. The reading is the RTC's own digits,
 * not the time: see nextboot.h for why that makes the zone irrelevant.
 *
 * THE LOADER'S ONLY DISK WRITE IS HERE. Everything else falcon-bios does to
 * a disk is a read (disk.c), and this file is linked into the stick's stage 3
 * alone: stage 2 and the stationary build (DESIGN Section 13) carry no code
 * that can write a disk at all, rather than code that happens not to be
 * called.
 */

#include <marker.h>

#include <media/nextboot.h>
#include <lib/string.h>

_Static_assert(NEXTBOOT_LBA >= STAGE3_LBA + STAGE3_MAX_SECTORS,
               "the marker sector must be clear of the stages");
_Static_assert(NEXTBOOT_LBA < 2048,
               "the marker sector must be in front of the first partition");

static uint8_t g_sec[NEXTBOOT_SECTOR_BYTES];

/* One sector, INT 13h AH=43h, through the bounce buffer disk.c reads
 * through. AH=43h is in the same subset of the extensions as the AH=42h
 * every read already depends on, so a BIOS that reads the stick through them
 * writes it through them too. AL=0, no verify: the caller reads the sector
 * back instead, which checks what the stick holds rather than what the BIOS
 * says it did. */
static int write_sector(const bios_disk *d, uint64_t lba, const uint8_t *buf)
{
    memcpy((void *)(uintptr_t)BOUNCE_ADDR, buf, NEXTBOOT_SECTOR_BYTES);

    bios_dap_at.size = 16;
    bios_dap_at.reserved = 0;
    bios_dap_at.sectors = 1;
    bios_dap_at.offset = (uint16_t)(BOUNCE_ADDR & 0x0F);
    bios_dap_at.segment = (uint16_t)(BOUNCE_ADDR >> 4);
    bios_dap_at.lba = lba;

    thunk_regs.eax = 0x4300u;
    thunk_regs.edx = d->drive;
    thunk_regs.esi = (uint32_t)DAP_ADDR;
    thunk_regs.es = 0;
    thunk_vector = 0x13;
    thunk_bios();

    return (thunk_regs.flags & BIOS_FLAG_CARRY) == 0 ? 0 : -1;
}

/* INT 1Ah AH=04h: CH century, CL year, DH month, DL day, all BCD. Carry when
 * the clock is not running. */
static int rtc_date(uint8_t out[4])
{
    thunk_regs.eax = 0x0400u;
    thunk_regs.ecx = 0;
    thunk_regs.edx = 0;
    thunk_vector = 0x1A;
    thunk_bios();
    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return -1;
    out[0] = (uint8_t)(thunk_regs.ecx >> 8);
    out[1] = (uint8_t)thunk_regs.ecx;
    out[2] = (uint8_t)(thunk_regs.edx >> 8);
    out[3] = (uint8_t)thunk_regs.edx;
    return 0;
}

/* INT 1Ah AH=02h: CH hour, CL minute, DH second, all BCD. */
static int rtc_time(uint8_t out[3])
{
    thunk_regs.eax = 0x0200u;
    thunk_regs.ecx = 0;
    thunk_regs.edx = 0;
    thunk_vector = 0x1A;
    thunk_bios();
    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return -1;
    out[0] = (uint8_t)(thunk_regs.ecx >> 8);
    out[1] = (uint8_t)thunk_regs.ecx;
    out[2] = (uint8_t)(thunk_regs.edx >> 8);
    return 0;
}

/* The RTC's reading in seconds, by nextboot_rtc_seconds's rule. The date is
 * read on both sides of the time, and the time again if the date moved, so
 * a read across midnight cannot pair one day's date with the next day's
 * clock. */
static int rtc_now(uint64_t *now)
{
    uint8_t d1[4];
    uint8_t d2[4];
    uint8_t t[3];
    uint8_t bcd[7];

    if (rtc_date(d1) != 0 || rtc_time(t) != 0 || rtc_date(d2) != 0)
        return -1;
    if (memcmp(d1, d2, sizeof d1) != 0 && rtc_time(t) != 0)
        return -1;
    memcpy(bcd, d2, 4);
    memcpy(bcd + 4, t, 3);
    return nextboot_rtc_seconds(bcd, now) == NEXTBOOT_OK ? 0 : -1;
}

static void log_state(const char *what)
{
    bios_puts("falcon-bios: next-boot marker: ");
    bios_puts(what);
    bios_puts("\n");
}

marker_state marker_take(const bios_disk *d, char *out, size_t cap)
{
    uint64_t stamp = 0;
    uint64_t now = 0;
    nextboot_sector parsed;
    size_t i;

    if (out != (char *)0 && cap != 0)
        out[0] = '\0';
    /* The LBA is a count of 512-byte sectors on both sides. A disk that
     * reports another size is not one a writer of this format addressed. */
    if (d == (const bios_disk *)0 || d->sector_size != NEXTBOOT_SECTOR_BYTES)
        return MARKER_NONE;
    if (bios_disk_read((void *)(uintptr_t)d,
                       (uint64_t)NEXTBOOT_LBA * NEXTBOOT_SECTOR_BYTES,
                       sizeof g_sec, g_sec) != 0)
        return MARKER_NONE;
    if (!nextboot_sector_is_marker(g_sec, sizeof g_sec))
        return MARKER_NONE;

    /* Parsed into this function's own copy, then the sector is cleared, and
     * only then is anything decided. */
    parsed = nextboot_sector_parse(g_sec, sizeof g_sec, &stamp, out, cap);

    memset(g_sec, 0, sizeof g_sec);
    if (write_sector(d, NEXTBOOT_LBA, g_sec) != 0 ||
        bios_disk_read((void *)(uintptr_t)d,
                       (uint64_t)NEXTBOOT_LBA * NEXTBOOT_SECTOR_BYTES,
                       sizeof g_sec, g_sec) != 0) {
        log_state("could not be cleared");
        if (out != (char *)0 && cap != 0)
            out[0] = '\0';
        return MARKER_STUCK;
    }
    for (i = 0; i < sizeof g_sec; i++) {
        if (g_sec[i] != 0u) {
            log_state("the clear did not take");
            if (out != (char *)0 && cap != 0)
                out[0] = '\0';
            return MARKER_STUCK;
        }
    }

    if (parsed != NEXTBOOT_SECTOR_VALUE) {
        log_state("cleared; it held nothing usable");
        return MARKER_BAD;
    }
    if (rtc_now(&now) != 0) {
        log_state("cleared; the clock did not answer");
        out[0] = '\0';
        return MARKER_NO_CLOCK;
    }
    bios_puts("falcon-bios: next-boot marker: set at ");
    bios_put_dec(stamp);
    bios_puts(", now ");
    bios_put_dec(now);
    bios_puts("\n");
    if (nextboot_age_of(stamp, now, NEXTBOOT_WINDOW_SECONDS) !=
        NEXTBOOT_AGE_FRESH) {
        log_state("cleared; not set within the window");
        out[0] = '\0';
        return MARKER_STALE;
    }
    bios_puts("falcon-bios: next-boot marker: cleared; names ");
    bios_puts(out);
    bios_puts("\n");
    return MARKER_VALUE;
}
