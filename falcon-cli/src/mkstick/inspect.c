/* falcon-boot mkstick: `status`, read-only inspection of a stick.
 *
 * The point of this command is to answer "will the loader recognise this stick,
 * and why not" without booting anything. So it deliberately runs the LOADER'S
 * OWN parsers rather than a second opinion: falcon-core/src/part/table.c reads
 * the table (MBR or GPT), falcon-core/src/ident the identity records,
 * falcon-core/src/luks the LUKS2 headers and tokens, falcon-core/src/fs the
 * filesystems. All are EFI-free by construction (every device access is a
 * byte-read callback), so the code that answers here is the code that will
 * answer at boot.
 *
 * What a partition is comes from its identity record (DESIGN Section 3.1),
 * never from its number or its type: the boot partition's record names the
 * install, and every other partition is judged by whether it carries the
 * same install ID. The BIOS stages are found the way the loaders find them:
 * stage 2 where stage 1 says, stage 3 where the boot record says.
 *
 * It writes nothing and opens nothing for writing. Listing the ESP's files
 * is a read-only loop mount, which needs root; without it, it says so rather
 * than reporting a pass it did not perform.
 */

#include <mkstick/mkstick.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <common/log.h>
#include <common/jmem.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/bioscore.h>
#include <mkstick/cleanup.h>
#include <mkstick/disk.h>
#include <mkstick/records.h>
#include <mkstick/shimname.h>

#include <fs/fs.h>
#include <ident/ident.h>
#include <luks/luks.h>
#include <media/nextboot.h>
#include <part/part.h>
#include <part/table.h>

/* Where stage 1 keeps stage 2's location (DESIGN Section 3.3). */
#include <falcon-bios/src/start/biosdefs.h>

static const cli_opt status_opts[] = {
    { "no-esp", CLI_OPT_FLAG, 0, NULL,
      "skip the p1 listing, which is the only check that needs root" }
};

typedef struct {
    unsigned ok;
    unsigned warn;
    unsigned fail;
    unsigned skip;
} tally;

static void check(tally *t, int good, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void check(tally *t, int good, const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    if (good)
        t->ok++;
    else
        t->fail++;
    log_info("  %-4s %s", good ? "OK" : "FAIL", line);
}

static void note(tally *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void note(tally *t, const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    t->warn++;
    log_info("  %-4s %s", "WARN", line);
}

static void skip(tally *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void skip(tally *t, const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    t->skip++;
    log_info("  %-4s %s", "--", line);
}

static void guid_text(const uint8_t *g, char *out, size_t cap)
{
    (void)snprintf(out, cap,
                   "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                   "%02x%02x%02x%02x%02x%02x",
                   g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
                   g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* The GPT entry's name, rendered for a human. Lossy on purpose: it stops at
 * the first NUL and turns anything outside printable ASCII into '?', so a
 * hostile name off somebody's disk cannot scribble on this report. Nothing
 * ever matches on the result - that is gpt_name_eq's job, over the raw
 * units. */
static void name_text(const part_entry *p, char *out, size_t cap)
{
    size_t n = 0;
    unsigned i;

    for (i = 0; i < GPT_NAME_LEN && n + 1u < cap; i++) {
        uint16_t u = p->name[i];

        if (u == 0)
            break;
        out[n++] = (u >= 0x20u && u < 0x7Fu) ? (char)u : '?';
    }
    out[n] = '\0';
}

/* ---- p3 and p4: LUKS2 headers ---- */

static const char *kdf_name(luks_kdf_type k)
{
    switch (k) {
    case LUKS_KDF_PBKDF2:   return "pbkdf2";
    case LUKS_KDF_ARGON2I:  return "argon2i";
    case LUKS_KDF_ARGON2ID: return "argon2id";
    case LUKS_KDF_UNKNOWN:  break;
    }
    return "unknown";
}

void mks_slot_describe(const luks_slot_info *info, char *buf, size_t cap)
{
    if (info->kdf == LUKS_KDF_PBKDF2) {
        (void)snprintf(buf, cap, "pbkdf2, %u iterations",
                       info->kdf_iterations);
    } else if (info->kdf == LUKS_KDF_ARGON2ID ||
               info->kdf == LUKS_KDF_ARGON2I) {
        uint32_t kib = info->kdf_memory_kib;

        if (kib % 1024u == 0u)
            (void)snprintf(buf, cap, "%s, time %u, %u MiB, %u lane%s",
                           kdf_name(info->kdf), info->kdf_time, kib / 1024u,
                           info->kdf_lanes,
                           info->kdf_lanes == 1u ? "" : "s");
        else
            (void)snprintf(buf, cap, "%s, time %u, %u KiB, %u lane%s",
                           kdf_name(info->kdf), info->kdf_time, kib,
                           info->kdf_lanes,
                           info->kdf_lanes == 1u ? "" : "s");
    } else {
        (void)snprintf(buf, cap, "%s", kdf_name(info->kdf));
    }
}

static void check_luks(tally *t, const char *path, const part_entry *p,
                       uint32_t sector, int loader_opens)
{
    static char json[LUKS_JSON_CAP];
    disk_io io;
    luks_vol vol;
    size_t slots = 0;
    size_t loader_slots = 0;
    size_t i;
    int rc;

    if (disk_io_open(&io, path, p->start_lba * sector,
                     (p->end_lba - p->start_lba + 1u) * sector) != CLI_OK) {
        check(t, 0, "%s", cli_error_msg());
        return;
    }

    rc = luks_open(&vol, disk_io_read, &io, json, sizeof json);
    if (rc != LUKS_OK) {
        check(t, 0, "no readable LUKS2 header (luks_open returned %d)", rc);
        disk_io_close(&io);
        return;
    }
    check(t, 1, "LUKS2 header, sequence id %llu, %u-byte sectors",
          (unsigned long long)vol.seqid, vol.seg_sector_size);

    (void)luks_slot_count(&vol, &slots);
    if (slots == 0)
        check(t, 0, "no keyslots: nothing can ever open this partition");

    for (i = 0; i < slots; i++) {
        luks_slot_info info;
        char kdf[96];

        if (luks_slot_at(&vol, i, &info) != LUKS_OK) {
            check(t, 0, "keyslot %zu could not be read", i);
            continue;
        }
        mks_slot_describe(&info, kdf, sizeof kdf);
        if (loader_opens) {
            /* p3 is the partition the LOADERS open, so it needs at least one
             * keyslot falcon-core's unlock can process (DESIGN Section 3).
             * argon2id and pbkdf2 both are; argon2i and anything unknown
             * are not. A slot above the stick's fixed cost opens, but not
             * on every machine the stick is meant to boot: falcon-bios on a
             * small one refuses it for want of memory. */
            if (info.usable) {
                check(t, 1, "keyslot %u: %s, %u-byte key", info.id, kdf,
                      info.key_size);
                loader_slots++;
            } else {
                note(t, "keyslot %u: %s, %u-byte key; the loaders cannot "
                        "use it", info.id, kdf, info.key_size);
            }
            if (info.kdf == LUKS_KDF_ARGON2ID &&
                info.kdf_memory_kib > (uint32_t)MKS_STICK_KDF_MEMORY_KIB)
                note(t, "keyslot %u needs %u MiB, above the stick's %u MiB: "
                        "a machine with less free memory refuses it",
                     info.id, (info.kdf_memory_kib + 1023u) / 1024u,
                     (unsigned)MKS_STICK_KDF_MEMORY_KIB / 1024u);
            if (info.kdf == LUKS_KDF_PBKDF2 &&
                info.kdf_iterations > 4u * 200000u)
                note(t, "keyslot %u costs %u iterations; unlock will be slow "
                        "in the loaders", info.id, info.kdf_iterations);
        } else {
            /* p4 is opened by the initramfs, never by a loader, so
             * cryptsetup's own argon2id cost is what it should have. */
            check(t, info.kdf == LUKS_KDF_ARGON2ID ||
                     info.kdf == LUKS_KDF_ARGON2I,
                  "keyslot %u: %s (opened by the initramfs)", info.id, kdf);
        }
    }
    if (loader_opens && slots != 0 && loader_slots == 0)
        check(t, 0, "no argon2id or pbkdf2 keyslot: the loaders cannot open "
                    "this shelf");
    disk_io_close(&io);
}

/* ---- p3 without LUKS (DESIGN Section 3's --p3-plain) ----
 *
 * Read through the loader's own filesystem registry, for the same reason the
 * rest of this file uses the loader's parsers: the question is not "what does
 * this partition hold" but "what will the boot menu make of it", and the only
 * way to answer that is with the code that will be asked. */

/* Counts what is in the UKI directory and reports the two facts that decide
 * whether this shelf can boot anything: a *.efi to launch, and the private
 * key NOT being here, which on a plaintext partition is a requirement rather
 * than a detail (DESIGN Section 7.5). */
static void check_plain_shelf(tally *t, disk_io *io)
{
    static fs_vol vol;
    static fs_dir root;
    static fs_dir uki;
    fs_dirent ent;
    const fs_ops *ops;
    unsigned efis = 0;
    int found_uki = 0;
    int found_key = 0;
    fs_jinfo jinfo;
    int rc;

    /* Stated as a WARN rather than a pass: the stick works, and the user gave
     * something up to get it. DESIGN Section 3 asks for that to be said out
     * loud wherever the stick is described. */
    note(t, "p3: PLAINTEXT (no signing appliance possible)");
    note(t, "     blessing and UKI signing are external-host only, from the "
            "--key-out copy");

    ops = fs_identify(disk_io_read, io, FS_HINT_SHELF);
    if (ops == NULL) {
        check(t, 0, "no filesystem here that this loader recognises");
        return;
    }
    rc = fs_mount_journal(&vol, ops, disk_io_read, io, &cli_jmem, &jinfo);
    if (rc != FS_OK) {
        check(t, 0, "%s volume does not mount (fs_mount returned %d)",
              ops->name, rc);
        return;
    }
    check(t, 1, "%s volume mounts", fs_vol_name(&vol));
    if ((jinfo.flags & FS_JNL_REPLAYED) != 0)
        note(t, "p3: read through its journal (an OS has it mounted, is "
                "hibernated, or lost power)");

    if (fs_dir_open_root(&vol, &root) != FS_OK) {
        check(t, 0, "the root directory could not be read");
        return;
    }
    while ((rc = fs_dir_next(&root, &ent)) == 1) {
        if (ent.is_dir && fs_name_eq_ascii(ent.name, MKS_P3_UKI_DIR)) {
            found_uki = (fs_dir_open(&vol, &uki, &ent) == FS_OK);
            continue;
        }
        if (!ent.is_dir && fs_name_eq_ascii(ent.name, MKS_P3_KEY_NAME))
            found_key = 1;
    }
    if (rc < 0)
        check(t, 0, "reading the root directory failed (%d)", rc);

    check(t, found_uki, "/%s present", MKS_P3_UKI_DIR);
    if (found_uki) {
        while ((rc = fs_dir_next(&uki, &ent)) == 1) {
            if (ent.is_regular && fs_name_ends_ascii(ent.name, ".efi"))
                efis++;
        }
        if (rc < 0)
            check(t, 0, "reading /%s failed (%d)", MKS_P3_UKI_DIR, rc);
        else
            check(t, efis > 0, "%u *.efi image%s in /%s", efis,
                  (efis == 1u) ? "" : "s", MKS_P3_UKI_DIR);
    }

    /* The one thing that must NOT be here. A private key on a partition
     * anybody can read is the key model broken, not a configuration choice,
     * so it is a failure however the stick was built. */
    check(t, !found_key,
          "no %s on the plaintext shelf (a private key here would be "
          "readable by anyone holding the stick)", MKS_P3_KEY_NAME);
}

/* ---- p2: the ISO library ---- */

static void check_iso_library(tally *t, const char *path,
                              const part_entry *p,
                              uint32_t sector)
{
    static fs_vol vol;
    static fs_dir dir;
    disk_io io;
    fs_dirent e;
    const fs_ops *ops;
    unsigned isos = 0;
    int rc;

    if (disk_io_open(&io, path, p->start_lba * sector,
                     (p->end_lba - p->start_lba + 1u) * sector) != CLI_OK) {
        check(t, 0, "%s", cli_error_msg());
        return;
    }

    ops = fs_identify(disk_io_read, &io, FS_HINT_ISO);
    if (ops == NULL || fs_mount(&vol, ops, disk_io_read, &io) != FS_OK) {
        check(t, 0, "no filesystem the loader reads");
        disk_io_close(&io);
        return;
    }
    check(t, 1, "%s volume mounts", ops->name);
    /* The library is the drag-and-drop target and WinPE's way to the ISO;
     * a filesystem neither reads leaves the stick half working. */
    check(t, ops->extents != NULL,
          "%s resolves a file to its extents (DESIGN Section 6.2)",
          ops->name);

    if (fs_dir_open_root(&vol, &dir) != FS_OK) {
        check(t, 0, "the root directory could not be read");
        disk_io_close(&io);
        return;
    }
    /* The count tells the user whether the library is populated; the
     * contents are theirs, so it is not a check. */
    while ((rc = fs_dir_next(&dir, &e)) == 1) {
        if (!e.is_dir && fs_name_ends_ascii(e.name, ".iso"))
            isos++;
    }
    if (rc < 0)
        check(t, 0, "reading the root directory failed (%d)", rc);

    rc = fs_lookup(&vol, "/" MKS_P2_BLESSED_DIR, &dir, &e);
    check(t, rc == FS_OK && e.is_dir, "/%s present", MKS_P2_BLESSED_DIR);

    log_info("  %-4s %u ISO%s in the library", "--", isos,
             (isos == 1u) ? "" : "s");
    disk_io_close(&io);
}

/* ---- the ESP ----
 *
 * The only check that needs privileges: the files are listed through a
 * read-only loop mount of the boot partition, because this is what the
 * firmware reads, and the kernel's FAT is the reader nearest to it.
 */

static void esp_file(tally *t, const char *mnt, const char *rel,
                     const char *what)
{
    char path[MKS_PATH_MAX];
    char size[32];
    struct stat st;

    if (util_join(path, sizeof path, mnt, rel) != CLI_OK) {
        check(t, 0, "%s: path too long", rel);
        return;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        check(t, 0, "%s missing (%s)", rel, what);
        return;
    }
    check(t, 1, "%-24s %10s  %s", rel,
          util_format_size((uint64_t)st.st_size, size, sizeof size), what);
}

static void status_check_esp(tally *t, mks_disk *d, unsigned number)
{
    char tmpl[] = "/tmp/falcon-mkstick-esp.XXXXXX";
    char node[MKS_PATH_MAX];
    char shim_path[MKS_PATH_MAX];
    char next_stage[SHIM_NEXT_STAGE_MAX];
    char mm_name[SHIM_NEXT_STAGE_MAX];
    char rel[MKS_PATH_MAX];
    int mounted = 0;

    if (geteuid() != 0) {
        skip(t, "listing the ESP needs root: it is loop-mounted read-only");
        return;
    }
    if (mkdtemp(tmpl) == NULL) {
        check(t, 0, "could not create a mount point: %s", strerror(errno));
        return;
    }

    if (disk_attach(d) != CLI_OK ||
        disk_partition_name(d->base, number, node, sizeof node) != CLI_OK ||
        disk_settle_node(node) != CLI_OK) {
        check(t, 0, "%s", cli_error_msg());
        goto out;
    }
    if (proc_runl_quiet("mount", "-o", "ro", node, tmpl, PROC_END) != CLI_OK) {
        check(t, 0, "%s", cli_error_msg());
        goto out;
    }
    if (cleanup_push_mount(tmpl) != CLI_OK) {
        (void)proc_runl_quiet("umount", tmpl, PROC_END);
        check(t, 0, "%s", cli_error_msg());
        goto out;
    }
    mounted = 1;

    esp_file(t, tmpl, MKS_ESP_BOOT_DIR "/" MKS_ESP_SHIM_NAME,
             "shim, the removable-media default the firmware verifies");

    /* The loader's filename on p1 is not a constant: it is whatever THIS shim
     * has compiled in, so the check reads it out of the shim we actually
     * installed rather than asserting a name (DESIGN Section 4). */
    if (util_join(shim_path, sizeof shim_path, tmpl,
                  MKS_ESP_BOOT_DIR "/" MKS_ESP_SHIM_NAME) == CLI_OK &&
        util_is_file(shim_path)) {
        if (shim_next_stage_name(shim_path, next_stage,
                                 sizeof next_stage) == CLI_OK) {
            (void)snprintf(rel, sizeof rel, "%s/%s", MKS_ESP_BOOT_DIR,
                           next_stage);
            esp_file(t, tmpl, rel,
                     "our loader, under the name shim launches");
        } else {
            check(t, 0, "%s", cli_error_msg());
        }
        if (shim_mokmanager_name(shim_path, mm_name,
                                 sizeof mm_name) == CLI_OK) {
            (void)snprintf(rel, sizeof rel, "%s/%s", MKS_ESP_BOOT_DIR,
                           mm_name);
            esp_file(t, tmpl, rel, "MokManager, shim's enrollment UI");
        }
    }

    esp_file(t, tmpl, MKS_ESP_CERT_NAME,
             "our PUBLIC certificate, for first-boot enrollment");

out:
    if (mounted && cleanup_pop() != CLI_OK)
        note(t, "%s", cli_error_msg());
    if (d->loop_attached && disk_detach(d) != CLI_OK)
        note(t, "%s", cli_error_msg());
    (void)rmdir(tmpl);
}

/* ---- the BIOS stages ----
 *
 * Found the way the loaders find them, not at numbers written here: stage 1
 * carries stage 2's LBA and length, stage 2 starts with the gate whose magic
 * ("BOS2", four bytes in) stage 1 checks, and stage 3 starts with "BOSZ" at
 * the sector the boot record names. Read by magic rather than compared with
 * a build: status is read-only and runs where no build may be. */

static void check_bios(tally *t, const mks_disk *d, const mks_records *r,
                       int boot)
{
    uint8_t mbr[BIOS_SECTOR_BYTES];
    uint8_t sec[BIOS_SECTOR_BYTES];
    uint64_t s2_lba = 0;
    uint64_t s2_n = 0;
    uint64_t first = 0;
    uint32_t i;
    disk_io io;

    if (d->sector != BIOS_SECTOR_BYTES && d->sector != BIOS_SECTOR_MAX) {
        skip(t, "%u-byte sectors: the BIOS route reads 512- and 4096-byte "
             "ones", d->sector);
        return;
    }
    if (disk_io_open(&io, d->path, 0, d->size) != CLI_OK) {
        check(t, 0, "%s", cli_error_msg());
        return;
    }
    if (disk_io_read(&io, 0, sizeof mbr, mbr) != 0) {
        check(t, 0, "LBA 0 could not be read");
        disk_io_close(&io);
        return;
    }
    if (!bioscore_stage1_fields(mbr, STAGE1_S2_LBA_OFF, STAGE1_S2_SECTORS_OFF,
                                &s2_lba, &s2_n)) {
        note(t, "not installed (LBA 0 names no stage 2); `mkstick bios` puts "
                "it on");
        disk_io_close(&io);
        return;
    }
    /* The first partition stage 2 must end before is the first real one:
     * `mkstick bios --reserve`'s partition covers stage 2 on purpose. */
    for (i = 0; i < r->table.count; i++) {
        if (!bioscore_is_reserve(&r->table, &r->parts[i]) &&
            (first == 0u || r->parts[i].start_lba < first))
            first = r->parts[i].start_lba;
    }
    check(t, 1, "stage 1 in LBA 0 names stage 2 at LBA %llu, %llu sector(s)",
          (unsigned long long)s2_lba, (unsigned long long)s2_n);
    check(t, s2_lba + s2_n <= first,
          "stage 2 ends before the first partition (LBA %llu)",
          (unsigned long long)first);
    check(t, disk_io_read(&io, s2_lba * d->sector, sizeof sec,
                          sec) == 0 && memcmp(sec + 4, "BOS2", 4) == 0,
          "stage 2 is at LBA %llu", (unsigned long long)s2_lba);

    if (boot < 0) {
        check(t, 0, "no boot partition to hold stage 3");
    } else {
        const part_entry *bp = &r->parts[boot];
        const ident_rec *rec = &r->recs[boot].rec;
        uint64_t s3 = bp->start_lba + rec->stage3_sector;

        check(t, disk_io_read(&io, s3 * d->sector, sizeof sec,
                              sec) == 0 && memcmp(sec, "BOSZ", 4) == 0,
              "stage 3 is at sector %u of p%u (LBA %llu)",
              (unsigned)rec->stage3_sector, bp->number,
              (unsigned long long)s3);
        if (rec->marker_sector != 0u) {
            /* The whole sector of the disk's size, as the loader and
             * next-boot judge it: 4096 bytes on a 4Kn disk. */
            static uint8_t msec[NEXTBOOT_SECTOR_MAX];
            uint64_t m = bp->start_lba + rec->marker_sector;
            size_t k;
            int zero = 1;

            if (!nextboot_sector_size_ok(d->sector) ||
                disk_io_read(&io, m * d->sector, d->sector, msec) != 0) {
                check(t, 0, "the marker sector could not be read");
            } else {
                for (k = 0; k < d->sector; k++)
                    zero &= (msec[k] == 0u);
                if (zero)
                    log_info("  %-4s next-boot marker (p%u sector %u): empty",
                             "--", bp->number, (unsigned)rec->marker_sector);
                else if (nextboot_sector_is_marker(msec, d->sector))
                    note(t, "next-boot marker (p%u sector %u) is set",
                         bp->number, (unsigned)rec->marker_sector);
                else
                    check(t, 0, "the marker sector (p%u sector %u) holds "
                                "something that is not a marker",
                          bp->number, (unsigned)rec->marker_sector);
            }
        }
    }
    disk_io_close(&io);
}

/* ---- the command ---- */

static void type_text(const part_table *t, const part_entry *p, char *out,
                      size_t cap)
{
    if (t->id.kind == PART_KIND_MBR) {
        (void)snprintf(out, cap, "MBR type 0x%02x", p->mbr_type);
        return;
    }
    if (gpt_guid_eq(p->type_guid, gpt_guid_esp))
        (void)snprintf(out, cap, "EFI system");
    else if (gpt_guid_eq(p->type_guid, falcon_guid_boot))
        (void)snprintf(out, cap, "falcon boot");
    else if (gpt_guid_eq(p->type_guid, falcon_guid_shelf))
        (void)snprintf(out, cap, "falcon shelf");
    else if (gpt_guid_eq(p->type_guid, gpt_guid_msbasic))
        (void)snprintf(out, cap, "Microsoft basic data");
    else if (bioscore_is_reserve(t, p))
        (void)snprintf(out, cap, "BIOS boot (stage 2)");
    else
        guid_text(p->type_guid, out, cap);
}

static cli_status status_run(const cli_args *a)
{
    static mks_records r;
    mks_disk disk;
    tally t;
    cli_status rc;
    uint32_t i;
    int boot;
    int roles[4] = { 0, 0, 0, 0 };
    char idhex[33];
    char dtext[PART_DISK_ID_TEXT];

    memset(&t, 0, sizeof t);

    rc = mkstick_guid_selfcheck();
    if (rc != CLI_OK)
        return rc;
    rc = disk_stat(&disk, a->pos[0]);
    if (rc != CLI_OK)
        return rc;
    disk_describe(&disk);

    rc = records_read(&r, disk.path, disk.sector, disk.size);
    if (rc != CLI_OK)
        return rc;
    if (r.rc != PART_OK)
        return set_error(CLI_ERR_INVALID,
                         "%s carries no usable partition table "
                         "(part_read_table returned %d)", disk.path, r.rc);

    part_disk_id_format(&r.table.id, dtext);
    cli_log_blank();
    log_info("partitions (DESIGN Sections 3.1, 3.2): %s", dtext);
    if (r.table.used_backup)
        note(&t, "the primary GPT failed its CRC; read from the backup");

    /* The boot partition names the install; everything else is measured
     * against it. */
    boot = records_boot(&r);
    if (boot >= 0) {
        records_id_hex(r.recs[boot].rec.install_id, idhex);
        check(&t, 1, "install ID %s (from p%u's boot record)", idhex,
              r.parts[boot].number);
    } else {
        check(&t, 0, "no partition carries a boot record; nothing names "
                     "this install");
    }

    for (i = 0; i < r.table.count; i++) {
        const part_entry *p = &r.parts[i];
        const mks_rec *rec = &r.recs[i];
        char size[32];
        char type[48];
        char name[GPT_NAME_LEN + 1u];
        char line[256];
        uint64_t bytes = (p->end_lba - p->start_lba + 1u) * disk.sector;

        type_text(&r.table, p, type, sizeof type);
        name_text(p, name, sizeof name);
        (void)snprintf(line, sizeof line, "p%-2u LBA %-10llu %10s  %-20s %s",
                       p->number, (unsigned long long)p->start_lba,
                       util_format_size(bytes, size, sizeof size), type,
                       name);

        if (rec->state == REC_OK) {
            char hex[33];

            records_id_hex(rec->rec.install_id, hex);
            if ((unsigned)rec->rec.role < 4u)
                roles[rec->rec.role]++;
            if (boot >= 0 && !ident_same_install(&rec->rec,
                                                 &r.recs[boot].rec))
                check(&t, 0, "%s  %s record (%s), install ID %s: a "
                             "different install", line,
                      ident_role_name(rec->rec.role), rec->where, hex);
            else
                check(&t, 1, "%s  %s (%s)", line,
                      ident_role_name(rec->rec.role), rec->where);
        } else if (rec->state == REC_BAD) {
            check(&t, 0, "%s  a %s that does not decode", line,
                  rec->where != NULL ? rec->where : "record");
        } else {
            log_info("  %-4s %s  no record%s", "--", line,
                     rec->luks ? " (LUKS2)" : "");
        }
    }
    check(&t, roles[IDENT_ROLE_LIBRARY] >= 1, "an ISO library (role library)");
    /* A stick made --no-p3 has no shelf, and boots media alone. */
    if (roles[IDENT_ROLE_SHELF] >= 1)
        check(&t, 1, "a UKI shelf (role shelf)");
    else
        note(&t, "no UKI shelf: no personal OS and no signing appliance "
                 "(mkstick create --no-p3)");
    if (roles[IDENT_ROLE_BOOT] > 1 || roles[IDENT_ROLE_LIBRARY] > 1 ||
        roles[IDENT_ROLE_SHELF] > 1)
        note(&t, "a role appears on more than one partition; the loaders "
                 "take the first");

    for (i = 0; i < r.table.count; i++) {
        const part_entry *p = &r.parts[i];
        const mks_rec *rec = &r.recs[i];

        if (rec->state == REC_OK && rec->rec.role == IDENT_ROLE_LIBRARY) {
            cli_log_blank();
            log_info("p%u: the ISO library (DESIGN Sections 6.2, 8.1)",
                     p->number);
            check_iso_library(&t, disk.path, p, disk.sector);
        } else if (rec->state == REC_OK && rec->rec.role == IDENT_ROLE_SHELF) {
            cli_log_blank();
            log_info("p%u: the UKI shelf (DESIGN Sections 3, 5)", p->number);
            if (rec->luks) {
                check_luks(&t, disk.path, p, disk.sector, 1);
            } else {
                disk_io shelf_io;

                if (disk_io_open(&shelf_io, disk.path,
                                 p->start_lba * disk.sector,
                                 (p->end_lba - p->start_lba + 1u) *
                                     disk.sector) != CLI_OK) {
                    check(&t, 0, "%s", cli_error_msg());
                } else {
                    check_plain_shelf(&t, &shelf_io);
                    disk_io_close(&shelf_io);
                }
            }
        } else if (rec->state == REC_NONE && rec->luks) {
            /* A LUKS2 volume with no record is the root the personal OS
             * opens itself (p4 on a stick); the loaders never read it. */
            cli_log_blank();
            log_info("p%u: LUKS2 with no record (root and home)", p->number);
            check_luks(&t, disk.path, p, disk.sector, 0);
        }
    }

    cli_log_blank();
    log_info("falcon-bios (DESIGN Section 3.3)");
    check_bios(&t, &disk, &r, boot);

    cli_log_blank();
    log_info("the ESP and the Secure Boot chain (DESIGN Section 4)");
    if (boot < 0 || r.table.id.kind != PART_KIND_GPT ||
        !gpt_guid_eq(r.parts[boot].type_guid, gpt_guid_esp))
        check(&t, 0, "the boot partition is not an EFI System Partition");
    else if (cli_flag(a, "no-esp"))
        skip(&t, "skipped (--no-esp)");
    else
        status_check_esp(&t, &disk, r.parts[boot].number);

    cli_log_blank();
    log_info("%u checks passed, %u warnings, %u failures, %u skipped",
             t.ok, t.warn, t.fail, t.skip);

    if (t.fail > 0)
        return set_error(CLI_ERR_INVALID,
                         "%s is not a stick this loader would boot", disk.path);
    return CLI_OK;
}

const cli_cmd mks_cmd_status = {
    "status",
    "inspect a stick and report whether the loader would recognise it",
    "<target>",
    status_opts,
    sizeof status_opts / sizeof status_opts[0],
    1, 1,
    "Read-only. The table (MBR or GPT), the identity records, LUKS2 and the\n"
    "filesystems are read by the loader's own parsers (falcon-core/src/part,\n"
    "ident, luks, fs), so they answer the question the boot menu will ask.\n"
    "The install ID is the boot partition's; a partition carrying another is\n"
    "flagged. The BIOS stages are checked where stage 1 and the boot record\n"
    "say they are. Listing the ESP's files needs a read-only loop mount, and\n"
    "so needs root; --no-esp skips it.\n",
    status_run
};
