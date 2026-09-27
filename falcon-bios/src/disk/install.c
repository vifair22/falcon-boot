/* falcon-bios: which partitions are this install's. See install.h. */

#include <disk/install.h>

#include <fs/fs.h>
#include <ident/identfs.h>
#include <luks/luks.h>
#include <lib/string.h>

/* The BDA's count of fixed disks: how many drive numbers from 0x80 up the
 * BIOS answers for. */
#define BDA_NDISKS 0x475u

/* Caller-allocated, as everywhere in falcon-core, and static because stage
 * 3's stack is not the place for a 16 KB JSON area. Each probe reuses them;
 * nothing here is kept past the call. */
static uint8_t          g_sec[4096];
static luks_vol         g_vol;
static char             g_json[LUKS_JSON_CAP];
static ident_fs_scratch g_fs;

typedef struct {
    bios_disk *disk;
    uint64_t   base;
} part_src;

static part_src g_src;

/* bios_disk_bytes, not bios_disk_read: LUKS2 and the filesystem drivers
 * read parts of a sector (DESIGN Section 3.4). */
static int part_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const part_src *p = ctx;

    if (off > ~(uint64_t)0 - p->base)
        return -1;
    return bios_disk_bytes(p->disk, p->base + off, len, buf);
}

uint64_t install_boot_lba(void)
{
    return *(volatile const uint64_t *)(uintptr_t)GATE_BOOTPART;
}

uint16_t install_marker_sector(void)
{
    return *(volatile const uint16_t *)(uintptr_t)GATE_MARKER;
}

void install_put_id(const ident_rec *r)
{
    static const char hex[] = "0123456789abcdef";
    unsigned i;

    for (i = 0; i < IDENT_ID_LEN; i++) {
        bios_putc(hex[r->install_id[i] >> 4]);
        bios_putc(hex[r->install_id[i] & 0xFu]);
    }
}

install_state install_boot_record(bios_disk *boot, ident_rec *out)
{
    uint64_t lba = install_boot_lba();
    int rc;

    if (boot->sector_size > sizeof g_sec ||
        lba > ~(uint64_t)0 / boot->sector_size ||
        bios_disk_read(boot, lba * boot->sector_size, boot->sector_size,
                       g_sec) != 0) {
        bios_puts("falcon-bios: the boot partition's first sector would "
                  "not read\n");
        return INSTALL_UNREAD;
    }
    rc = ident_decode_bin(g_sec, out);
    if (rc == IDENT_ERR_NONE) {
        bios_puts("falcon-bios: no record on the boot partition\n");
        return INSTALL_NONE;
    }
    if (rc != IDENT_OK || out->role != IDENT_ROLE_BOOT) {
        bios_puts("falcon-bios: the boot partition's record is not a boot "
                  "record\n");
        return INSTALL_BAD;
    }
    bios_puts("falcon-bios: boot partition at LBA ");
    bios_put_dec(lba);
    bios_puts(" of drive ");
    bios_put_hex64(boot->drive);
    bios_puts(", install ");
    install_put_id(out);
    bios_puts("\n");
    return INSTALL_OK;
}

const char *install_problem(install_state s)
{
    switch (s) {
    case INSTALL_UNREAD:
        return "The partition this loader started from would not read,\n"
               "so no ISO library or shelf is offered.";
    case INSTALL_NONE:
        return "The partition this loader started from has no falcon "
               "record,\nso no ISO library or shelf is offered.\n"
               "Write the stick with a current mkstick.";
    case INSTALL_BAD:
        return "The falcon record on the partition this loader started "
               "from is\ndamaged or not a boot record, so no ISO library "
               "or shelf is offered.\nWrite the stick with a current "
               "mkstick.";
    case INSTALL_OK:
    default:
        return (const char *)0;
    }
}

unsigned install_drives(uint8_t boot, uint8_t *out, unsigned max)
{
    unsigned count = peek8(BDA_NDISKS);
    unsigned n = 0;
    unsigned i;

    if (max == 0)
        return 0;
    out[n++] = boot;
    for (i = 0; i < count && i < 0x80u && n < max; i++) {
        uint8_t drive = (uint8_t)(0x80u + i);

        if (drive != boot)
            out[n++] = drive;
    }
    return n;
}

int install_part_record(bios_disk *d, const part_entry *p, ident_rec *out,
                        int *encrypted)
{
    uint32_t version = 0;
    const char *role = (const char *)0;
    const char *id = (const char *)0;
    size_t role_len = 0;
    size_t id_len = 0;

    *encrypted = 0;
    if (d == (bios_disk *)0 || p == (const part_entry *)0 ||
        (p->primary && part_mbr_is_extended(p->mbr_type)))
        return 0;
    g_src.disk = d;
    g_src.base = p->start_lba * (uint64_t)d->sector_size;

    if (luks_open(&g_vol, part_read, &g_src, g_json, sizeof g_json) ==
        LUKS_OK) {
        *encrypted = 1;
        if (luks_falcon_token(&g_vol, &version, &role, &role_len, &id,
                              &id_len) != LUKS_OK)
            return 0;
        return ident_from_strings(version, role, role_len, id, id_len,
                                  out) == IDENT_OK;
    }
    /* A /.falcon at the root of whatever the registry mounts here. */
    return ident_probe(part_read, &g_src, &g_fs, out) == IDENT_OK;
}
