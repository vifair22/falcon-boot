/* falcon-boot mkstick: `ident`, the identity records as a script sees them
 * (docs/DESIGN.md Section 3.1).
 *
 * Three jobs, each small:
 *
 *   - list: one line per partition, number, role, install ID and start, so
 *     a script finds a partition by what it is rather than by its number
 *     (the rig and corpus builders mount p2 this way);
 *   - --write-boot: stamp the binary `boot` record into a FAT partition's
 *     boot sector (and its FAT32 backup), the one record a shell cannot
 *     write with printf, since it carries a CRC. The other two are text: a
 *     /.falcon line, and a LUKS2 token `cryptsetup token import` takes.
 *     --write-boot-at does the same for a FAT volume with no partition
 *     table around it, at a byte offset and with the sectors given: jump's
 *     floppy, whose stage 3 is not where a partition's is;
 *   - --reserved-sectors: the reserved sector count a `boot` partition is
 *     formatted with, from the rule `mkstick create` uses, so an image
 *     builder formats with the same number.
 *
 * Listing needs no root on an image file; it reads with the loaders' code.
 */

#include <mkstick/mkstick.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <common/log.h>
#include <mkstick/bioscore.h>
#include <mkstick/disk.h>
#include <mkstick/records.h>

#include <ident/ident.h>
#include <part/table.h>

#include <falcon-bios/src/start/biosdefs.h>

#define DEFAULT_STAGE3 "build/bin/falcon-bios-stage3.bin"

static const cli_opt ident_opts[] = {
    { "write-boot", CLI_OPT_VALUE, 0, "<n>",
      "write the boot record into partition n's FAT boot sector" },
    { "write-boot-at", CLI_OPT_VALUE, 0, "<byte>",
      "write the boot record into the FAT boot sector at this byte offset "
      "of an image with no partition table" },
    { "stage3-sector", CLI_OPT_VALUE, 0, "<n>",
      "with --write-boot-at: stage 3's first sector in the volume" },
    { "marker-sector", CLI_OPT_VALUE, 0, "<n>",
      "with --write-boot-at: the next-boot marker's sector, 0 for none" },
    { "id", CLI_OPT_VALUE, 0, "<hex|new>",
      "the install ID a boot record gets: 32 hex digits, or new" },
    { "reserved-sectors", CLI_OPT_FLAG, 0, NULL,
      "print the reserved sector count a boot partition is formatted with" },
    { "stage3", CLI_OPT_VALUE, 0, "<file>",
      "the stage 3 --reserved-sectors sizes for (default " DEFAULT_STAGE3
      ")" }
};

static cli_status print_reserved(const cli_args *a)
{
    const char *stage3 = cli_value(a, "stage3", DEFAULT_STAGE3);
    struct stat st;
    uint32_t n;

    if (stat(stage3, &st) != 0 || !S_ISREG(st.st_mode))
        return set_error_errno(CLI_ERR_NOTFOUND, "stat %s", stage3);
    n = bioscore_reserved_for(BOOTPART_STAGE3_SECTOR, (size_t)st.st_size);
    if (n == 0u)
        return set_error(CLI_ERR_RANGE,
                         "%s is too large for a FAT reserved area", stage3);
    (void)printf("%u\n", (unsigned)n);
    return CLI_OK;
}

/* --id's value into the record: 32 hex digits, or a fresh draw. */
static cli_status take_id(const cli_args *a, const char *opt, ident_rec *rec)
{
    const char *idtext = cli_value(a, "id", NULL);
    static const uint8_t zero[IDENT_ID_LEN];

    if (idtext == NULL)
        return set_error(CLI_ERR_USAGE, "--%s needs --id", opt);
    if (strcmp(idtext, "new") == 0)
        return records_new_id(rec->install_id);
    if (!records_id_parse(idtext, rec->install_id) ||
        memcmp(rec->install_id, zero, sizeof zero) == 0)
        return set_error(CLI_ERR_USAGE,
                         "--id %s: not 32 hex digits (not all zero) or "
                         "'new'", idtext);
    return CLI_OK;
}

/* A volume with no table: the offset and both sectors are the caller's,
 * since no rule of a partitioned disk says where jump's stage 3 is. */
static cli_status write_boot_at(const cli_args *a, const char *target)
{
    const char *why = NULL;
    uint64_t off = 0;
    ident_rec rec;
    char hex[33];
    mks_disk d;
    cli_status rc;

    if (cli_value(a, "write-boot", NULL) != NULL)
        return set_error(CLI_ERR_USAGE,
                         "--write-boot and --write-boot-at are two ways to "
                         "say where; give one");
    if (cli_value(a, "stage3-sector", NULL) == NULL ||
        cli_value(a, "marker-sector", NULL) == NULL)
        return set_error(CLI_ERR_USAGE, "--write-boot-at needs "
                         "--stage3-sector and --marker-sector");
    if (!records_boot_args(cli_value(a, "write-boot-at", NULL),
                           cli_value(a, "stage3-sector", NULL),
                           cli_value(a, "marker-sector", NULL), &off, &rec,
                           &why))
        return set_error(CLI_ERR_USAGE, "%s", why);
    rc = take_id(a, "write-boot-at", &rec);
    if (rc != CLI_OK)
        return rc;
    rc = disk_stat(&d, target);
    if (rc != CLI_OK)
        return rc;
    if (off >= d.size)
        return set_error(CLI_ERR_RANGE, "byte %llu is past the end of %s",
                         (unsigned long long)off, d.path);

    rc = records_write_boot(d.path, off, &rec);
    if (rc != CLI_OK)
        return rc;
    records_id_hex(rec.install_id, hex);
    log_info("%s, byte %llu: boot record, stage 3 at %u, marker at %u, "
             "install ID %s", d.path, (unsigned long long)off,
             (unsigned)rec.stage3_sector, (unsigned)rec.marker_sector, hex);
    (void)printf("%s\n", hex);
    return CLI_OK;
}

static cli_status write_boot(const cli_args *a, const char *target)
{
    static mks_records r;
    const char *which = cli_value(a, "write-boot", NULL);
    const part_entry *p = NULL;
    ident_rec rec;
    char hex[33];
    char *end = NULL;
    unsigned long n;
    uint32_t i;
    mks_disk d;
    cli_status rc;

    n = strtoul(which, &end, 10);
    if (end == which || *end != '\0' || n == 0u)
        return set_error(CLI_ERR_USAGE, "--write-boot %s: not a partition "
                         "number", which);
    if (cli_value(a, "stage3-sector", NULL) != NULL ||
        cli_value(a, "marker-sector", NULL) != NULL)
        return set_error(CLI_ERR_USAGE, "--write-boot writes stage 3 at %u "
                         "and the marker at %u; the sectors are "
                         "--write-boot-at's", (unsigned)BOOTPART_STAGE3_SECTOR,
                         (unsigned)BOOTPART_MARKER_SECTOR);

    memset(&rec, 0, sizeof rec);
    rec.version = IDENT_VERSION;
    rec.role = IDENT_ROLE_BOOT;
    rec.stage3_sector = BOOTPART_STAGE3_SECTOR;
    rec.marker_sector = BOOTPART_MARKER_SECTOR;
    rc = take_id(a, "write-boot", &rec);
    if (rc != CLI_OK)
        return rc;

    rc = disk_stat(&d, target);
    if (rc != CLI_OK)
        return rc;
    rc = records_read(&r, d.path, d.sector, d.size);
    if (rc != CLI_OK)
        return rc;
    if (r.rc != PART_OK)
        return set_error(CLI_ERR_INVALID, "%s has no usable partition table "
                         "(part_read_table %d)", d.path, r.rc);
    for (i = 0; i < r.table.count; i++) {
        if (r.parts[i].number == n)
            p = &r.parts[i];
    }
    if (p == NULL)
        return set_error(CLI_ERR_NOTFOUND, "%s has no partition %lu", d.path,
                         n);
    /* Only where a loader will look: stage 2 reads nothing else. */
    if (!records_boot_candidate(&r.table, p))
        return set_error(CLI_ERR_INVALID,
                         "p%lu of %s is not ESP-, falcon-boot- or FAT-typed; "
                         "a loader would never read a boot record there",
                         n, d.path);

    rc = records_write_boot(d.path, p->start_lba * d.sector, &rec);
    if (rc != CLI_OK)
        return rc;
    records_id_hex(rec.install_id, hex);
    log_info("p%lu of %s: boot record, install ID %s", n, d.path, hex);
    (void)printf("%s\n", hex);
    return CLI_OK;
}

static cli_status list(const char *target)
{
    static mks_records r;
    mks_disk d;
    uint32_t i;
    cli_status rc;

    rc = disk_stat(&d, target);
    if (rc != CLI_OK)
        return rc;
    rc = records_read(&r, d.path, d.sector, d.size);
    if (rc != CLI_OK)
        return rc;
    if (r.rc != PART_OK)
        return set_error(CLI_ERR_INVALID, "%s has no usable partition table "
                         "(part_read_table %d)", d.path, r.rc);

    /* number, role, install ID, first LBA: tab-separated, one partition per
     * line, "-" for a partition with no record and "bad" for one that does
     * not decode. The format is for scripts; keep it stable. */
    for (i = 0; i < r.table.count; i++) {
        const mks_rec *rec = &r.recs[i];
        char hex[33];

        if (rec->state == REC_OK) {
            records_id_hex(rec->rec.install_id, hex);
            (void)printf("%u\t%s\t%s\t%llu\n", r.parts[i].number,
                         ident_role_name(rec->rec.role), hex,
                         (unsigned long long)r.parts[i].start_lba);
        } else {
            (void)printf("%u\t%s\t-\t%llu\n", r.parts[i].number,
                         rec->state == REC_BAD ? "bad" : "-",
                         (unsigned long long)r.parts[i].start_lba);
        }
    }
    return CLI_OK;
}

static cli_status ident_run(const cli_args *a)
{
    const char *target = (a->npos > 0u) ? a->pos[0] : NULL;

    if (cli_flag(a, "reserved-sectors"))
        return print_reserved(a);
    if (target == NULL)
        return set_error(CLI_ERR_USAGE, "which disk or image?");
    if (cli_value(a, "write-boot-at", NULL) != NULL)
        return write_boot_at(a, target);
    if (cli_value(a, "write-boot", NULL) != NULL)
        return write_boot(a, target);
    if (cli_value(a, "id", NULL) != NULL ||
        cli_value(a, "stage3-sector", NULL) != NULL ||
        cli_value(a, "marker-sector", NULL) != NULL)
        return set_error(CLI_ERR_USAGE,
                         "--id and the sectors are for --write-boot and "
                         "--write-boot-at");
    return list(target);
}

const cli_cmd mks_cmd_ident = {
    "ident",
    "list the identity records on a disk, or write a boot record",
    "[<target>]",
    ident_opts,
    sizeof ident_opts / sizeof ident_opts[0],
    0, 1,
    "With a target alone, prints one line per partition, tab-separated:\n"
    "number, role (boot, library, shelf; - for none, bad for a record that\n"
    "does not decode), install ID and first LBA. Scripts find a partition by\n"
    "its role this way rather than by its number.\n"
    "\n"
    "--write-boot <n> --id <hex|new> writes the boot record (DESIGN Section\n"
    "3.1: stage 3 at sector 32, the marker at sector 8) into partition n's\n"
    "FAT boot sector and its FAT32 backup, and prints the install ID. The\n"
    "partition must be ESP-, falcon-boot- or FAT-typed and formatted with\n"
    "at least that many reserved sectors (--reserved-sectors).\n"
    "\n"
    "--write-boot-at <byte> --stage3-sector <n> --marker-sector <n>\n"
    "--id <hex|new> writes the boot record into the FAT boot sector at that\n"
    "byte offset of an image with no partition table (jump's floppy), with\n"
    "the sectors given (relative to the volume, the marker 0 for none).\n"
    "\n"
    "--reserved-sectors prints the count `mkstick create` formats p1 with:\n"
    "stage 3's first sector plus twice --stage3's size, at least 1 MiB.\n",
    ident_run
};
