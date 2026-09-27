/* falcon-boot mkstick: `status`, read-only inspection of a stick.
 *
 * The point of this command is to answer "will the loader recognise this stick,
 * and why not" without booting anything. So it deliberately runs the LOADER'S
 * OWN parsers rather than a second opinion: falcon-core/src/part/part.c reads
 * the GPT, falcon-core/src/luks reads the LUKS2 headers,
 * falcon-core/src/fs/exfat.c reads p2. All three are EFI-free by construction
 * (every device access is a byte-read callback), so the code that answers here
 * is the code that will answer at boot. A separate implementation could agree
 * with the disk and still disagree with the loader, which is the only
 * disagreement that matters.
 *
 * It writes nothing, opens nothing for writing, and needs no privileges for
 * three of the four partitions. p1 is the exception: FAT is the one
 * filesystem this repo has no reader for (the firmware supplies it at boot),
 * so listing the ESP means a read-only loop mount, and that needs root. When
 * it cannot, it says so rather than reporting a pass it did not perform.
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
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/bioscore.h>
#include <mkstick/cleanup.h>
#include <mkstick/disk.h>
#include <mkstick/shimname.h>

#include <fs/exfat.h>
#include <fs/fs.h>
#include <luks/luks.h>
#include <part/part.h>

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
static void name_text(const gpt_part *p, char *out, size_t cap)
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

static void check_luks(tally *t, const char *path, const gpt_part *p,
                       uint32_t sector, int want_pbkdf2)
{
    static char json[LUKS_JSON_CAP];
    disk_io io;
    luks_vol vol;
    size_t slots = 0;
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

        if (luks_slot_at(&vol, i, &info) != LUKS_OK) {
            check(t, 0, "keyslot %zu could not be read", i);
            continue;
        }
        if (want_pbkdf2) {
            /* p3 is the partition the LOADER opens, so its keyslot has to be
             * one the in-EFI unlock can process at all (DESIGN Section 3),
             * and at an iteration count budgeted for that implementation
             * rather than for cryptsetup's host benchmark (Section 8). */
            check(t, info.kdf == LUKS_KDF_PBKDF2 && info.usable,
                  "keyslot %u: %s, %u iterations, %u-byte key%s",
                  info.id, kdf_name(info.kdf), info.kdf_iterations,
                  info.key_size,
                  (info.kdf == LUKS_KDF_PBKDF2 && info.usable)
                      ? "" : "  <- the loader cannot open this");
            if (info.kdf == LUKS_KDF_PBKDF2 &&
                info.kdf_iterations > 4u * 200000u)
                note(t, "keyslot %u costs %u iterations; unlock will be slow "
                        "in EFI-land", info.id, info.kdf_iterations);
        } else {
            /* p4 is opened by the initramfs, never by EFI, so argon2id is
             * what it should have and "the loader cannot use this" is the
             * correct outcome rather than a fault. */
            check(t, info.kdf == LUKS_KDF_ARGON2ID ||
                     info.kdf == LUKS_KDF_ARGON2I,
                  "keyslot %u: %s (opened by the initramfs, not by EFI)",
                  info.id, kdf_name(info.kdf));
        }
    }
    disk_io_close(&io);
}

/* ---- p3 without LUKS (DESIGN Section 3's --p3-plain) ----
 *
 * Read through the loader's own filesystem registry, for the same reason the
 * rest of this file uses the loader's parsers: the question is not "what does
 * this partition hold" but "what will the boot menu make of it", and the only
 * way to answer that is with the code that will be asked. */

static const uint8_t luks2_magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

static int shelf_is_luks(disk_io *io)
{
    uint8_t blk[512];

    if (disk_io_read(io, 0, sizeof blk, blk) != 0)
        return 0;
    return memcmp(blk, luks2_magic, sizeof luks2_magic) == 0;
}

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
    rc = fs_mount(&vol, ops, disk_io_read, io);
    if (rc != FS_OK) {
        check(t, 0, "%s volume does not mount (fs_mount returned %d)",
              ops->name, rc);
        return;
    }
    check(t, 1, "%s volume mounts", fs_vol_name(&vol));

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

/* ---- p2: the exFAT ISO library ---- */

static int name_is(const exfat_dirent *e, const char *ascii)
{
    size_t len = strlen(ascii);
    size_t i;

    if ((size_t)e->name_len != len)
        return 0;
    for (i = 0; i < len; i++) {
        uint16_t a = e->name[i];
        uint16_t b = (uint16_t)ascii[i];

        if (a >= 'A' && a <= 'Z')
            a = (uint16_t)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (uint16_t)(b + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

/* Does this entry's name end in ".iso"? The library's contents are
 * drag-and-dropped by the user, so the count is informational, not a check. */
static int name_is_iso_suffix(const exfat_dirent *e)
{
    static const char suffix[] = ".iso";
    size_t n = (size_t)e->name_len;
    size_t i;

    if (n < 4u)
        return 0;
    for (i = 0; i < 4u; i++) {
        uint16_t c = e->name[n - 4u + i];

        if (c >= 'A' && c <= 'Z')
            c = (uint16_t)(c + 32);
        if (c != (uint16_t)suffix[i])
            return 0;
    }
    return 1;
}

static int find_child(exfat_vol *v, exfat_dir *d, const char *name,
                      exfat_dirent *out)
{
    exfat_dirent e;
    int rc;

    (void)v;
    while ((rc = exfat_dir_next(d, &e)) == 1) {
        if (name_is(&e, name)) {
            *out = e;
            return 1;
        }
    }
    return (rc < 0) ? rc : 0;
}

static void check_iso_library(tally *t, const char *path, const gpt_part *p,
                              uint32_t sector)
{
    static exfat_vol vol;
    static exfat_dir dir;
    static exfat_dir sub;
    disk_io io;
    exfat_dirent falcon = { { 0 }, 0, 0, 0, 0, 0, 0 };
    exfat_dirent blessed;
    unsigned isos = 0;
    int rc;

    if (disk_io_open(&io, path, p->start_lba * sector,
                     (p->end_lba - p->start_lba + 1u) * sector) != CLI_OK) {
        check(t, 0, "%s", cli_error_msg());
        return;
    }

    rc = exfat_mount(&vol, disk_io_read, &io);
    if (rc != 0) {
        check(t, 0, "no readable exFAT volume (exfat_mount returned %d)", rc);
        disk_io_close(&io);
        return;
    }
    check(t, 1, "exFAT volume mounts");

    if (exfat_dir_open_root(&vol, &dir) != 0) {
        check(t, 0, "the root directory could not be read");
        disk_io_close(&io);
        return;
    }

    /* Count ISOs while looking for the reserved directory: one pass, and the
     * count is what tells the user whether the library is actually populated. */
    {
        exfat_dirent e;
        int found_falcon = 0;

        while ((rc = exfat_dir_next(&dir, &e)) == 1) {
            if (name_is(&e, "falcon") && e.is_dir) {
                falcon = e;
                found_falcon = 1;
            } else if (!e.is_dir && e.name_len > 4u &&
                       name_is_iso_suffix(&e)) {
                isos++;
            }
        }
        if (rc < 0)
            check(t, 0, "reading the root directory failed (%d)", rc);
        check(t, found_falcon, "/%s present", "falcon");
        if (!found_falcon) {
            disk_io_close(&io);
            return;
        }
    }

    if (exfat_dir_open(&vol, &sub, &falcon) != 0 ||
        find_child(&vol, &sub, "blessed", &blessed) != 1)
        check(t, 0, "/%s missing", MKS_P2_BLESSED_DIR);
    else
        check(t, blessed.is_dir != 0, "/%s present", MKS_P2_BLESSED_DIR);

    log_info("  %-4s %u ISO%s in the library", "--", isos,
             (isos == 1u) ? "" : "s");
    disk_io_close(&io);
}

/* ---- p1: the ESP ----
 *
 * The only check that needs privileges, and the only one that goes through
 * the kernel rather than through a parser in this repo, because FAT is what
 * the firmware reads for us at boot and there is no in-repo reader to reuse.
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

static void status_check_esp(tally *t, mks_disk *d)
{
    char tmpl[] = "/tmp/falcon-mkstick-esp.XXXXXX";
    char shim_path[MKS_PATH_MAX];
    char next_stage[SHIM_NEXT_STAGE_MAX];
    char mm_name[SHIM_NEXT_STAGE_MAX];
    char rel[MKS_PATH_MAX];
    int mounted = 0;

    if (geteuid() != 0) {
        skip(t, "listing p1 needs root: FAT is the one filesystem this repo "
                "has no reader for, so it has to be loop-mounted");
        return;
    }
    if (mkdtemp(tmpl) == NULL) {
        check(t, 0, "could not create a mount point: %s", strerror(errno));
        return;
    }

    if (disk_attach(d) != CLI_OK || disk_settle(d) != CLI_OK) {
        check(t, 0, "%s", cli_error_msg());
        goto out;
    }
    if (proc_runl_quiet("mount", "-o", "ro", d->part[0], tmpl, PROC_END) != CLI_OK) {
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
 * Read by their magic numbers rather than compared with a build: status is
 * read-only and runs where no build may be. Stage 2 carries "BOS2" four bytes
 * in (what stage 1 checks before it jumps) and stage 3 starts with "BOSZ"
 * (what stage 2 checks), at the LBAs falcon-bios/src/start/biosdefs.h fixes.
 * Nonzero when both are there. */
#define STATUS_STAGE2_LBA 34u
#define STATUS_STAGE3_LBA 98u

static int bios_stages_present(const mks_disk *d)
{
    uint8_t s2[512];
    uint8_t s3[512];
    disk_io io;
    int ok;

    if (d->sector != 512u)
        return 0;
    if (disk_io_open(&io, d->path, 0, d->size) != CLI_OK)
        return 0;
    ok = disk_io_read(&io, (uint64_t)STATUS_STAGE2_LBA * 512u, sizeof s2,
                      s2) == 0 &&
         disk_io_read(&io, (uint64_t)STATUS_STAGE3_LBA * 512u, sizeof s3,
                      s3) == 0 &&
         memcmp(s2 + 4, "BOS2", 4) == 0 && memcmp(s3, "BOSZ", 4) == 0;
    disk_io_close(&io);
    return ok;
}

/* ---- the command ---- */

/* p2 is named as well as typed, so this takes the whole entry: its Microsoft
 * basic data type is shared with every Windows data partition and identifies
 * nothing by itself (DESIGN Section 3). */
static const char *role_text(const gpt_part *p)
{
    if (gpt_part_is_role(p, PART_ROLE_ISO))
        return "falcon ISO library";
    if (gpt_part_is_role(p, PART_ROLE_SHELF))
        return "falcon UKI shelf";
    if (gpt_guid_eq(p->type, gpt_guid_esp))
        return "EFI system partition";
    return NULL;
}

static cli_status status_run(const cli_args *a)
{
    gpt_part parts[32];
    gpt_table table;
    mks_disk disk;
    disk_io io;
    tally t;
    cli_status rc;
    int gpt_rc;
    uint32_t i;
    uint32_t reserved = 0;
    int has_esp = 0;

    memset(&t, 0, sizeof t);

    rc = mkstick_guid_selfcheck();
    if (rc != CLI_OK)
        return rc;
    rc = disk_stat(&disk, a->pos[0]);
    if (rc != CLI_OK)
        return rc;
    disk_describe(&disk);

    memset(&table, 0, sizeof table);
    table.parts = parts;
    table.cap = (uint32_t)(sizeof parts / sizeof parts[0]);

    rc = disk_io_open(&io, disk.path, 0, disk.size);
    if (rc != CLI_OK)
        return rc;
    gpt_rc = gpt_parse(&table, disk_io_read, &io, disk.sector);
    disk_io_close(&io);

    if (gpt_rc != GPT_OK)
        return set_error(CLI_ERR_INVALID,
                         "%s carries no usable GPT (gpt_parse returned %d)",
                         disk.path, gpt_rc);

    cli_log_blank();
    log_info("GPT (DESIGN Section 3)");
    /* `mkstick bios --reserve`'s partition over the gap is this tool's own
     * and not one of the four, so it is not counted against them. */
    for (i = 0; i < table.count; i++) {
        if (bioscore_is_reserve(&table.parts[i]))
            reserved++;
        if (gpt_guid_eq(table.parts[i].type, gpt_guid_esp))
            has_esp = 1;
    }
    if (table.count - reserved != MKS_PARTS)
        note(&t, "%u partitions, not the four DESIGN Section 3 describes",
             table.count - reserved);
    for (i = 0; i < table.count; i++) {
        const gpt_part *p = &table.parts[i];
        const char *role = role_text(p);
        char guid[40];
        char size[32];
        uint64_t bytes = (p->end_lba - p->start_lba + 1u) * disk.sector;

        char name[GPT_NAME_LEN + 1u];

        guid_text(p->unique, guid, sizeof guid);
        /* The name is reported for every partition, not just p2, because p2's
         * name is now part of what identifies it: when the ISO library check
         * below fails, the first question is what the name actually says. */
        name_text(p, name, sizeof name);
        if (role != NULL)
            check(&t, 1, "p%u  %-22s %10s  name %-12s PARTUUID %s", p->number,
                  role, util_format_size(bytes, size, sizeof size), name,
                  guid);
        else if (bioscore_is_reserve(p))
            check(&t, 1, "p%u  %-22s %10s  name %-12s PARTUUID %s", p->number,
                  "falcon-bios's gap", util_format_size(bytes, size, sizeof size),
                  name, guid);
        else {
            char type[40];

            guid_text(p->type, type, sizeof type);
            check(&t, p->number == 4u,
                  "p%u  %-22s %10s  name %-12s PARTUUID %s", p->number,
                  (p->number == 4u) ? "Linux LUKS (root)" : type,
                  util_format_size(bytes, size, sizeof size), name, guid);
        }
    }
    check(&t, gpt_has_role(&table, PART_ROLE_ISO),
          "the ISO library is Microsoft-basic-data typed and named %s (the "
          "loader matches on that pair)", MKS_LABEL_P2);
    check(&t, gpt_has_role(&table, PART_ROLE_SHELF),
          "a falcon-typed UKI shelf is present (an unrecognised type, so "
          "Windows never offers to format it)");

    for (i = 0; i < table.count; i++) {
        const gpt_part *p = &table.parts[i];

        if (gpt_part_is_role(p, PART_ROLE_ISO)) {
            cli_log_blank();
            log_info("p%u: the exFAT ISO library (DESIGN Sections 6.2, 8.1)",
                     p->number);
            check_iso_library(&t, disk.path, p, disk.sector);
        } else if (gpt_part_is_role(p, PART_ROLE_SHELF)) {
            disk_io shelf_io;

            cli_log_blank();
            log_info("p%u: the UKI shelf (DESIGN Sections 3, 5)", p->number);
            if (disk_io_open(&shelf_io, disk.path, p->start_lba * disk.sector,
                             (p->end_lba - p->start_lba + 1u) * disk.sector)
                != CLI_OK) {
                check(&t, 0, "%s", cli_error_msg());
            } else {
                /* Which of the two shapes DESIGN Section 3 allows this is,
                 * decided the same way the loader decides it: by looking for
                 * the LUKS2 magic, not by trusting a flag somewhere. */
                int encrypted = shelf_is_luks(&shelf_io);

                if (encrypted) {
                    disk_io_close(&shelf_io);
                    check_luks(&t, disk.path, p, disk.sector, 1);
                } else {
                    check_plain_shelf(&t, &shelf_io);
                    disk_io_close(&shelf_io);
                }
            }
        } else if (p->number == 4u) {
            cli_log_blank();
            log_info("p%u: LUKS2 root and home", p->number);
            check_luks(&t, disk.path, p, disk.sector, 0);
        }
    }

    cli_log_blank();
    log_info("falcon-bios (docs/notes/bios-route.md)");
    if (bios_stages_present(&disk))
        check(&t, 1, "stages 2 and 3 are in the gap (LBA %u and %u)",
              STATUS_STAGE2_LBA, STATUS_STAGE3_LBA);
    else
        note(&t, "not installed; `mkstick bios` puts it on");

    cli_log_blank();
    log_info("p1: the ESP and the Secure Boot chain (DESIGN Section 4)");
    if (!has_esp)
        check(&t, 0, "no EFI System Partition on the stick");
    else if (cli_flag(a, "no-esp"))
        skip(&t, "skipped (--no-esp)");
    else
        status_check_esp(&t, &disk);

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
    "Read-only. The GPT, LUKS2 and exFAT checks are performed by the loader's\n"
    "own parsers (falcon-core/src/part/part.c, falcon-core/src/luks,\n"
    "falcon-core/src/fs/exfat.c), so they answer the question the boot menu\n"
    "will ask. Listing p1 needs a read-only loop mount, and so\n"
    "needs root; --no-esp skips it.\n",
    status_run
};
