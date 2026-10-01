/* falcon-bios: the read callback, over INT 13h.
 *
 * The route's per-platform boundary is this read callback, so falcon-core links unchanged
 * and a BIOS build is its fourth consumer after the host tests, the CLI
 * tools and the appliance. Everything above this - GPT, exFAT, ISO9660, UDF,
 * FAT, the El Torito catalog, the medium's volume set - is the same code the
 * UEFI loader runs.
 *
 * THE BOUNCE BUFFER IS NOT OPTIONAL, and it is the only thing here that is
 * not obvious. Real mode addresses twenty bits, so the BIOS cannot write
 * above one megabyte, and a caller in protected mode will happily pass a
 * buffer anywhere in the 4 GiB it can address. Every transfer therefore
 * lands in a fixed low buffer first and is copied up. Syslinux's MEMDISK and
 * every other BIOS loader does the same thing for the same reason.
 */

#include <start/bios.h>

#include <lib/string.h>

/* INT 13h AH=48h, the drive parameters. Same reachability rule.
 *
 * Not volatile. The BIOS writes it, but it does so inside thunk_bios, which
 * is a call the compiler already has to assume clobbers memory. */
typedef struct {
    uint16_t size;
    uint16_t flags;
    uint32_t cylinders;
    uint32_t heads;
    uint32_t sectors_per_track;
    uint64_t total_sectors;
    uint16_t bytes_per_sector;
} bios_driveparm;

#define g_params (*(bios_driveparm *)(uintptr_t)DRIVEPARM_ADDR)

/* bios_disk_on_call's function, in .data for the reason g_track below is. */
static void (*g_on_call)(void) __attribute__((section(".data"))) =
    (void (*)(void))0;

void bios_disk_on_call(void (*fn)(void))
{
    g_on_call = fn;
}

static void called(void)
{
    if (g_on_call != (void (*)(void))0)
        g_on_call();
}

/* One AH=02h call: `n` sectors from `at`, all on one track, to the real-mode
 * address `dest`. Three tries with a reset between, since a floppy's first
 * read can fail while its motor comes up to speed; that is what every BIOS's
 * own boot code does too. */
static int chs_call(const bios_disk *d, uint32_t at, uint32_t n,
                    uint32_t dest)
{
    uint16_t cyl;
    uint8_t head;
    uint8_t sector;
    int tries;

    if (vfd_lba_to_chs(&d->geom, at, &cyl, &head, &sector) != VFD_OK)
        return 0;
    for (tries = 0; tries < 3; tries++) {
        thunk_regs.eax = 0x0200u | n;          /* AH=02h, AL sectors */
        thunk_regs.ecx = ((uint32_t)(cyl & 0xFFu) << 8) |
                         (uint32_t)(sector & 0x3Fu) |
                         (uint32_t)((cyl >> 2) & 0xC0u);
        thunk_regs.edx = ((uint32_t)head << 8) | d->drive;
        thunk_regs.es = (uint16_t)(dest >> 4);
        thunk_regs.ebx = dest & 0x0Fu;
        thunk_vector = 0x13;
        thunk_bios();
        if ((thunk_regs.flags & BIOS_FLAG_CARRY) == 0) {
            called();
            return 1;
        }
        thunk_regs.eax = 0x0000u;              /* AH=00h, reset */
        thunk_regs.edx = d->drive;
        thunk_vector = 0x13;
        thunk_bios();
    }
    return 0;
}

/* The track cache (biosdefs.h, TRACK_CACHE_ADDR): which drive's which track
 * is there. Not in .bss, because stage 2 links this file and stage 2 has no
 * .bss (nothing clears it); a stale answer is ruled out by the valid flag
 * starting at zero in .data and by the drive and geometry being compared. */
static struct {
    uint32_t valid;
    uint32_t drive;
    uint32_t track;
    uint32_t sectors;
} g_track __attribute__((section(".data"))) = { 0, 0, 0, 0 };

void bios_disk_forget(void)
{
    g_track.valid = 0;
}

/* AH=02h, a track at a time, for a drive with no extensions: a read that
 * crosses a track is refused by many floppy controllers, and one sector per
 * call costs a revolution each.
 *
 * A read that ends before its track does (a FAT walk's single sectors, a
 * header before its body) reads the whole track into the cache instead and
 * copies out of it, so the next read on that track costs no revolution at
 * all. A read that runs to the end of its track goes straight to the bounce
 * buffer, as it did before there was a cache: it gains nothing from one. */
static int read_sectors_chs(const bios_disk *d, uint64_t lba, uint16_t count)
{
    uint32_t done = 0;
    uint32_t spt = d->geom.sectors;

    if (lba > 0xFFFFFFFFu || spt == 0 ||
        spt * d->sector_size > TRACK_CACHE_BYTES)
        return 0;
    while (done < count) {
        uint32_t at = (uint32_t)lba + done;
        uint32_t dest = (uint32_t)BOUNCE_ADDR + done * d->sector_size;
        uint32_t track = at / spt;
        uint32_t into = at % spt;
        uint32_t n;

        if (vfd_run_on_track(&d->geom, at, count - done, &n) != VFD_OK)
            return 0;
        if (g_track.valid && g_track.drive == d->drive &&
            g_track.track == track && g_track.sectors == spt) {
            memcpy((void *)(uintptr_t)dest,
                   (const void *)(uintptr_t)(TRACK_CACHE_ADDR +
                                             into * d->sector_size),
                   (size_t)n * d->sector_size);
        } else if (into + n < spt) {
            g_track.valid = 0;
            if (!chs_call(d, track * spt, spt, TRACK_CACHE_ADDR))
                return 0;
            g_track.valid = 1;
            g_track.drive = d->drive;
            g_track.track = track;
            g_track.sectors = spt;
            memcpy((void *)(uintptr_t)dest,
                   (const void *)(uintptr_t)(TRACK_CACHE_ADDR +
                                             into * d->sector_size),
                   (size_t)n * d->sector_size);
        } else if (!chs_call(d, at, n, dest)) {
            return 0;
        }
        done += n;
    }
    return 1;
}

static int read_sectors(const bios_disk *d, uint64_t lba, uint16_t count)
{
    if (d->geom.sectors != 0)
        return read_sectors_chs(d, lba, count);

    bios_dap_at.size = 16;
    bios_dap_at.reserved = 0;
    bios_dap_at.sectors = count;
    bios_dap_at.offset = (uint16_t)(BOUNCE_ADDR & 0x0Fu);
    bios_dap_at.segment = (uint16_t)(BOUNCE_ADDR >> 4);
    bios_dap_at.lba = lba;

    thunk_regs.eax = 0x4200u;              /* AH=42h, extended read */
    thunk_regs.edx = d->drive;
    thunk_regs.esi = (uint32_t)DAP_ADDR;
    thunk_regs.es = 0;
    thunk_vector = 0x13;
    thunk_bios();

    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return 0;
    called();
    return 1;
}

int bios_disk_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const bios_disk *d = ctx;
    uint8_t *out = buf;
    uint32_t ss;

    if (d == (const bios_disk *)0 || buf == (void *)0)
        return -1;
    ss = d->sector_size;
    if (ss == 0)
        return -1;
    /* Whole sectors only, which is what INT 13h moves. A driver that wants
     * part of a sector is handed bios_disk_bytes (disk/bytes.c), which
     * re-blocks its reads into these. */
    if (off % ss != 0 || len % ss != 0)
        return -1;

    while (len > 0) {
        uint64_t chunk = len;
        uint16_t count;

        if (chunk > BOUNCE_BYTES)
            chunk = BOUNCE_BYTES;
        count = (uint16_t)(chunk / ss);
        if (count == 0)
            return -1;

        if (!read_sectors(d, off / ss, count))
            return -1;
        memcpy(out, (const void *)(uintptr_t)BOUNCE_ADDR, (size_t)chunk);

        out += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

/* A drive AH=48h did not answer. If AH=41h says it has the extensions after
 * all, it stays on AH=42h at 512 bytes, as before there was a CHS path;
 * otherwise AH=08h's geometry, and AH=02h from then on (DESIGN Section 13.3).
 * Nonzero when the drive answered either. */
static int probe_chs(bios_disk *d)
{
    thunk_regs.eax = 0x4100u;              /* AH=41h, extensions present? */
    thunk_regs.ebx = 0x55AAu;
    thunk_regs.edx = d->drive;
    thunk_vector = 0x13;
    thunk_bios();
    if ((thunk_regs.flags & BIOS_FLAG_CARRY) == 0 &&
        (thunk_regs.ebx & 0xFFFFu) == 0xAA55u)
        return 1;

    thunk_regs.eax = 0x0800u;              /* AH=08h, drive geometry */
    thunk_regs.edx = d->drive;
    thunk_regs.es = 0;
    thunk_regs.edi = 0;
    thunk_vector = 0x13;
    thunk_bios();
    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return 0;
    if (vfd_geometry_int13((uint8_t)((thunk_regs.ecx >> 8) & 0xFFu),
                           (uint8_t)(thunk_regs.ecx & 0xFFu),
                           (uint8_t)((thunk_regs.edx >> 8) & 0xFFu),
                           &d->geom) != VFD_OK) {
        memset(&d->geom, 0, sizeof d->geom);
        return 0;
    }
    return 1;
}

int bios_disk_probe(bios_disk *d, uint8_t drive)
{
    if (d == (bios_disk *)0)
        return 0;
    d->drive = drive;
    /* 512 unless the drive says otherwise. An El Torito boot from a CD
     * arrives with 2048, and reading it in 512-byte units would quietly
     * return the wrong bytes rather than fail. */
    d->sector_size = 512;

    memset(&g_params, 0, sizeof g_params);
    g_params.size = sizeof g_params;

    memset(&d->geom, 0, sizeof d->geom);

    thunk_regs.eax = 0x4800u;              /* AH=48h, drive parameters */
    thunk_regs.edx = drive;
    thunk_regs.esi = (uint32_t)DRIVEPARM_ADDR;
    thunk_regs.es = 0;
    thunk_vector = 0x13;
    thunk_bios();

    if ((thunk_regs.flags & BIOS_FLAG_CARRY) != 0)
        return probe_chs(d);
    if (g_params.bytes_per_sector == 512u || g_params.bytes_per_sector == 2048u ||
        g_params.bytes_per_sector == 4096u)
        d->sector_size = g_params.bytes_per_sector;
    return 1;
}
