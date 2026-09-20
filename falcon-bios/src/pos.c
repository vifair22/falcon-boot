/* falcon-bios: the personal OS, unwrapped (docs/notes/bios-route.md Section
 * 4.4; docs/DESIGN.md Section 5).
 *
 * A UKI is PE32+ and has no BIOS entry point, so the verb here is not
 * "chainload". It is unwrapped: `.linux` inside a UKI is a plain bzImage
 * with the same setup header linux.c already boots, `.initrd` is an
 * initramfs, and `.cmdline` is a command line. sd-stub never runs.
 *
 * WHAT SD-STUB DID THAT HAS TO BE REBUILT. Its one contribution to this
 * boot, beyond starting the kernel, is the credential handoff: it takes
 * <uki>.efi.extra.d/cryptsetup.passphrase.cred and synthesises a cpio
 * placing the file at /.extra/credentials/, concatenated onto the initrd, so
 * systemd in the initramfs unlocks p4 without a second prompt. That is a
 * cpio write, and media/cpio.c is already the writer this tree uses to
 * append falcon-hook. Same writer, different path and payload. systemd only
 * ever sees the finished cpio, so the single prompt survives the port.
 *
 * THREE THINGS DO NOT SURVIVE, and the note says so rather than leaving them
 * to be found on hardware:
 *
 *   - Nothing verifies the UKI. Its Authenticode signature is decorative
 *     here: there is no shim, no db, and LUKS2 with plain aes-xts carries no
 *     integrity. The gate is "encrypted and passphrase-gated", which is
 *     confidentiality plus a passphrase, not authenticity.
 *   - LoaderDevicePartUUID does not exist, because sd-stub sets it. An
 *     initrd leaning on systemd-gpt-auto-generator to find root will not.
 *   - Nothing is measured into PCR 11, so anything sealed to it stops
 *     unsealing.
 */

#include <bios.h>
#include <pos.h>
#include <prompt.h>
#include <linux.h>

#include <part/part.h>
#include <fs/fs.h>
#include <luks/luks.h>
#include <pe/pe.h>
#include <media/cpio.h>
#include <lib/string.h>

/* Where the shelf keeps its images, and the credential beside one
 * (DESIGN Section 5.1). */
#define UKI_DIR      "uki"
#define CRED_DIR     ".extra"
#define CRED_SUBDIR  ".extra/credentials"
#define CRED_FILE    "cryptsetup.passphrase.cred"
#define CRED_PATH    CRED_SUBDIR "/" CRED_FILE

#define POS_MAX_UKIS 16u
#define PASS_MAX     256u
/* Enough of a PE to hold its headers and section table. */
#define PE_HDR_MAX   8192u

typedef struct {
    bios_disk *disk;
    uint64_t   base;
} part_src;

static part_src  g_src;
static luks_vol  g_vol;
static luks_dev  g_dev;
static char      g_json[LUKS_JSON_CAP];
static fs_vol    g_fs;
static fs_dir    g_dir;
static fs_dir    g_scratch;
static fs_file   g_file;
static int       g_opened;

static fs_dirent g_ukis[POS_MAX_UKIS];
static unsigned  g_uki_count;

static uint8_t g_pehdr[PE_HDR_MAX];
static uint8_t g_credcpio[PASS_MAX + 1024u];
static char    g_cmdline[2048];

/* The one buffer in this build that holds the passphrase. Wiped on every
 * path out, which is why every path out goes through one place. */
static char   g_pass[PASS_MAX];
static size_t g_pass_len;

static luks_master_key g_mk;

static void wipe_secrets(void)
{
    memset(g_pass, 0, sizeof g_pass);
    g_pass_len = 0;
    memset(&g_mk, 0, sizeof g_mk);
    memset(g_credcpio, 0, sizeof g_credcpio);
    luks_dev_close(&g_dev);
}

static void put(const char *s) { bios_screen_puts(s); }

static int part_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const part_src *p = ctx;

    if (off > ~(uint64_t)0 - p->base)
        return -1;
    return bios_disk_read(p->disk, p->base + off, len, buf);
}

int pos_probe(bios_disk *d, const gpt_part *p3)
{
    if (d == (bios_disk *)0 || p3 == (const gpt_part *)0)
        return 0;
    g_src.disk = d;
    g_src.base = p3->start_lba * (uint64_t)d->sector_size;
    g_opened = luks_open(&g_vol, part_read, &g_src, g_json, sizeof g_json) ==
               LUKS_OK;
    return g_opened;
}

/* The shelf's UKI directory, which DESIGN Section 5.1 puts at /uki. */
static int open_uki_dir(void)
{
    fs_dirent ent;

    if (fs_dir_open_root(&g_fs, &g_scratch) != FS_OK)
        return -1;
    while (fs_dir_next(&g_scratch, &ent) == 1) {
        if (ent.is_dir && fs_name_eq_ascii(ent.name, UKI_DIR))
            return fs_dir_open(&g_fs, &g_dir, &ent) == FS_OK ? 0 : -1;
    }
    /* No /uki: the images may be at the root. */
    return fs_dir_open_root(&g_fs, &g_dir) == FS_OK ? 0 : -1;
}

static void list_ukis(void)
{
    fs_dirent ent;

    g_uki_count = 0;
    while (fs_dir_next(&g_dir, &ent) == 1) {
        if (ent.is_dir || !fs_name_ends_ascii(ent.name, ".efi"))
            continue;
        if (g_uki_count < POS_MAX_UKIS)
            g_ukis[g_uki_count++] = ent;
    }
}

static void put_name(const uint16_t *name)
{
    char one[2];
    size_t i;

    one[1] = '\0';
    for (i = 0; name[i] != 0; i++) {
        one[0] = bios_ascii(name[i], '?');
        put(one);
    }
}

/* The credential sd-stub would have packed, as the cpio it would have made.
 *
 * Its content is the passphrase that just opened p3, because that is the one
 * that opens p4 as well (DESIGN Section 5.1). The UEFI loader synthesises the
 * same thing: it publishes the UKI on a filesystem of its own with a drop-in
 * directory beside it holding the passphrase as a .cred file, and sd-stub
 * turns that into exactly this cpio. Here there is no sd-stub, so the cpio is
 * written directly and concatenated onto .initrd.
 *
 * Returns its length, or 0 when there is no passphrase to pack, which is not
 * an error: an image whose root needs none has none.
 */
static size_t build_credential(void)
{
    cpio_entry e[3];
    size_t wrote = 0;

    if (g_pass_len == 0)
        return 0;

    /* Every parent directory has to be named, because the kernel's unpacker
     * creates only what the archive names. */
    e[0].path = CRED_DIR;
    e[0].data = (const void *)0;
    e[0].len = 0;
    e[0].mode = CPIO_MODE_DIR;
    e[1].path = CRED_SUBDIR;
    e[1].data = (const void *)0;
    e[1].len = 0;
    e[1].mode = CPIO_MODE_DIR;
    e[2].path = CRED_PATH;
    e[2].data = g_pass;
    e[2].len = g_pass_len;
    /* 0600: it is a passphrase, and the initramfs it lands in is root's. */
    e[2].mode = 0100600u;

    if (cpio_write(e, 3, g_credcpio, sizeof g_credcpio, &wrote) != CPIO_OK)
        return 0;
    return wrote;
}

/* A linux_object over a section of an open file. */
typedef struct {
    fs_file *f;
    uint64_t base;
} sect_src;

static sect_src g_linux_src;
static sect_src g_initrd_src;

static int sect_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const sect_src *s = ctx;

    return fs_file_read(s->f, s->base + off, len, buf) == FS_OK ? 0 : -1;
}

typedef struct {
    const uint8_t *at;
    uint64_t       len;
} mem_src;

static mem_src g_credsrc;

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const mem_src *m = ctx;

    if (off > m->len || len > m->len - off)
        return -1;
    memcpy(buf, m->at + off, (size_t)len);
    return 0;
}

static int boot_uki(const fs_dirent *uki)
{
    linux_object kernel;
    linux_object initrd[2];
    unsigned n_initrd = 0;
    pe_hdr hdr;
    size_t loff = 0;
    size_t llen = 0;
    size_t ioff = 0;
    size_t ilen = 0;
    size_t coff = 0;
    size_t clen = 0;
    size_t credlen;
    uint64_t total;
    size_t want;

    /* The size comes from the open file, not from the directory entry: an
     * ext4 entry carries no size (falcon-core/src/fs/fs.c says so where it
     * builds one), and reading zero bytes and then asking whether they are a
     * PE answers "that is not a UKI" about every image on the shelf. */
    if (fs_file_open(&g_fs, &g_file, uki) != FS_OK) {
        put("  the image would not open\n");
        return -1;
    }
    total = fs_file_size(&g_file);
    want = total < sizeof g_pehdr ? (size_t)total : sizeof g_pehdr;
    if (want == 0 || fs_file_read(&g_file, 0, want, g_pehdr) != FS_OK) {
        put("  the image would not read\n");
        return -1;
    }
    if (pe_parse(g_pehdr, want, &hdr) != PE_OK) {
        put("  that is not a UKI\n");
        return -1;
    }
    if (pe_find_section(&hdr, g_pehdr, (size_t)total, ".linux",
                        &loff, &llen) != PE_OK || llen == 0) {
        put("  the image has no .linux section; it is not a UKI\n");
        return -1;
    }
    (void)pe_find_section(&hdr, g_pehdr, (size_t)total, ".initrd",
                          &ioff, &ilen);
    (void)pe_find_section(&hdr, g_pehdr, (size_t)total, ".cmdline",
                          &coff, &clen);

    g_linux_src.f = &g_file;
    g_linux_src.base = loff;
    kernel.read = sect_read;
    kernel.ctx = &g_linux_src;
    kernel.size = llen;

    if (ilen > 0) {
        g_initrd_src.f = &g_file;
        g_initrd_src.base = ioff;
        initrd[0].read = sect_read;
        initrd[0].ctx = &g_initrd_src;
        initrd[0].size = ilen;
        n_initrd = 1;
    }

    /* The cmdline, out of the image rather than from anywhere else: it is
     * what the sign tool put there and what the UKI is. */
    g_cmdline[0] = '\0';
    if (clen > 0 && clen < sizeof g_cmdline) {
        if (fs_file_read(&g_file, coff, clen, g_cmdline) != FS_OK) {
            put("  the cmdline section would not read\n");
            return -1;
        }
        g_cmdline[clen] = '\0';
        /* The section is NUL-padded by ukify; stop at the first one. */
        {
            size_t i;

            for (i = 0; i < clen; i++) {
                if (g_cmdline[i] == '\0')
                    break;
            }
            g_cmdline[i] = '\0';
        }
    }

    credlen = build_credential();
    if (credlen > 0 && n_initrd < 2u) {
        g_credsrc.at = g_credcpio;
        g_credsrc.len = credlen;
        initrd[n_initrd].read = mem_read;
        initrd[n_initrd].ctx = &g_credsrc;
        initrd[n_initrd].size = credlen;
        n_initrd++;
        put("  passphrase credential packed, ");
    } else {
        put("  no passphrase credential: ");
    }
    put(credlen > 0 ? "p4 unlocks without a second prompt\n"
                    : "the initramfs will ask for its own\n");

    put("  starting the kernel\n");
    return linux_boot(&kernel, initrd, n_initrd, g_cmdline);
}

int pos_run(bios_disk *d, const gpt_part *p3)
{
    const fs_ops *ops;
    uint32_t slot = 0;
    unsigned sel = 0;
    int rc;

    if (!pos_probe(d, p3)) {
        put("  p3 is not a LUKS2 volume\n");
        return -1;
    }

    if (prompt_passphrase("passphrase", g_pass, sizeof g_pass,
                          &g_pass_len) != 0) {
        wipe_secrets();
        return -1;
    }

    rc = luks_unlock(&g_vol, g_pass, g_pass_len, &g_mk, &slot);
    if (rc != LUKS_OK) {
        if (rc == LUKS_ERR_NOKEY)
            put("  that passphrase does not open p3\n");
        else if (rc == LUKS_ERR_UNSUPPORTED_KDF)
            put("  p3's keyslots use a KDF this loader cannot run\n");
        else
            put("  p3 would not unlock\n");
        wipe_secrets();
        return -1;
    }
    if (luks_dev_open(&g_dev, &g_vol, &g_mk) != LUKS_OK) {
        put("  p3 unlocked but would not open\n");
        wipe_secrets();
        return -1;
    }

    ops = fs_identify(luks_read, &g_dev, FS_HINT_NONE);
    if (ops == (const fs_ops *)0 ||
        fs_mount(&g_fs, ops, luks_read, &g_dev) != FS_OK) {
        put("  nothing inside p3 mounted\n");
        wipe_secrets();
        return -1;
    }
    put("  p3 open, ");
    put(fs_vol_name(&g_fs));
    put("\n");

    if (open_uki_dir() != 0) {
        put("  p3 has no image directory\n");
        wipe_secrets();
        return -1;
    }
    list_ukis();
    if (g_uki_count == 0) {
        put("  p3 holds no images\n");
        wipe_secrets();
        return -1;
    }

    for (;;) {
        unsigned i;
        int key;

        bios_screen_clear();
        put("falcon-boot (BIOS)  personal OS\n\n");
        for (i = 0; i < g_uki_count; i++) {
            put(i == sel ? "  > " : "    ");
            put_name(g_ukis[i].name);
            put("\n");
        }
        put("\n  up and down to move, enter to boot, escape to go back\n");

        key = bios_getkey();
        if (key == BIOS_KEY_UP && sel > 0)
            sel--;
        else if (key == BIOS_KEY_DOWN && sel + 1u < g_uki_count)
            sel++;
        else if (key == BIOS_KEY_ESC)
            break;
        else if (key == BIOS_KEY_ENTER) {
            put("\n  ");
            put_name(g_ukis[sel].name);
            put("\n");
            (void)boot_uki(&g_ukis[sel]);
            (void)bios_getkey();
        }
    }

    wipe_secrets();
    return -1;
}
