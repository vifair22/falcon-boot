/* falcon-bios: the personal OS, unwrapped (docs/DESIGN.md Section 5).
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

#include <start/bios.h>
#include <pos/pos.h>
#include <console/prompt.h>
#include <linux/linux.h>
#include <console/screen.h>

#include <part/table.h>
#include <media/strows.h>
#include <fs/fs.h>
#include <luks/luks.h>
#include <pe/pe.h>
#include <crypto/crypto.h>
#include <crypto/xstate.h>
#include <crypto/lanes.h>
#include <mp/mp.h>
#include <mem/heap.h>
#include <mem/jnlmem.h>
#include <media/cpio.h>
#include <ident/ident.h>
#include <media/shelf.h>
#include <tui/tui.h>
#include <lib/string.h>

/* The credential beside the image (DESIGN Section 5.1). */
#define CRED_DIR     ".extra"
#define CRED_SUBDIR  ".extra/credentials"
#define CRED_FILE    "cryptsetup.passphrase.cred"
#define CRED_PATH    CRED_SUBDIR "/" CRED_FILE
#define IDENT_PATH   CRED_SUBDIR "/" IDENT_CRED

#define PASS_MAX     256u

/* Wrong passphrases this boot, across every prompt, as the UEFI loader
 * counts them (falcon-efi/src/pos/pos.c): TUI_UNLOCK_TRIES powers off. */
static unsigned g_failures;
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
static fs_file   g_file;
static int       g_opened;
static int       g_dev_open;
/* The shelf is plaintext (DESIGN Section 3, --p3-plain): mounted straight
 * off the partition, no passphrase, and so no credential. */
static int       g_plain;

/* The images on the shelf and what the chooser calls them, from the same
 * module the UEFI loader's chooser uses (falcon-core/src/media/shelf.h). */
static shelf_list g_images;
static const uint16_t *g_rows[SHELF_MAX];

/* What linux_boot is handed, summed, for the line the handoff prints. */
static uint64_t g_read;

static uint8_t g_pehdr[PE_HDR_MAX];
static uint8_t g_credcpio[PASS_MAX + 1024u];
static char    g_cmdline[2048];

/* The one buffer in this build that holds the passphrase. Wiped on every
 * path out, which is why every path out goes through one place. */
static char   g_pass[PASS_MAX];
static size_t g_pass_len;

static luks_master_key g_mk;

/* The boot partition's record as text, for IDENT_CRED; empty until the
 * caller has handed it over (pos_set_install). */
static char   g_ident[IDENT_TEXT_MAX];
static size_t g_ident_len;

/* Everything the unlock produced: the passphrase and the key, the
 * credential, the keyboard ring the passphrase was typed through, and the
 * plaintext caches read out of p3. crypto_wipe rather than memset because
 * these are exactly the dead-buffer stores an optimiser is entitled to drop.
 * Safe to call twice. */
static void wipe_secrets(void)
{
    crypto_wipe(g_pass, sizeof g_pass);
    g_pass_len = 0;
    crypto_wipe(&g_mk, sizeof g_mk);
    crypto_wipe(g_credcpio, sizeof g_credcpio);
    bios_key_buffer_wipe();
    shelf_wipe(&g_images);
    crypto_wipe(&g_fs, sizeof g_fs);
    crypto_wipe(&g_file, sizeof g_file);
    jnl_wipe();
    if (g_dev_open) {
        luks_dev_close(&g_dev);
        g_dev_open = 0;
    }
}

static int part_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const part_src *p = ctx;

    if (off > ~(uint64_t)0 - p->base)
        return -1;
    /* bios_disk_bytes, not bios_disk_read: the drivers above read parts of
     * a sector on a 4Kn disk (DESIGN Section 3.4). base is whole sectors,
     * so the layer sees the drivers' alignment unchanged. */
    return bios_disk_bytes(p->disk, p->base + off, len, buf);
}

/* What the last shelf mount did (fs_mount_journal), for the booting page and
 * a refusal's advice. */
static fs_jinfo g_jinfo;

/* A plaintext shelf's filesystem, mounted into g_fs. */
static int mount_plain(void)
{
    const fs_ops *ops = fs_identify(part_read, &g_src, FS_HINT_SHELF);

    return ops != (const fs_ops *)0 &&
           fs_mount_journal(&g_fs, ops, part_read, &g_src, &jnl_mem,
                            &g_jinfo) == FS_OK;
}

int pos_probe(bios_disk *d, const part_entry *shelf)
{
    if (d == (bios_disk *)0 || shelf == (const part_entry *)0)
        return 0;
    g_src.disk = d;
    g_src.base = shelf->start_lba * (uint64_t)d->sector_size;
    g_plain = 0;
    g_opened = luks_open(&g_vol, part_read, &g_src, g_json, sizeof g_json) ==
               LUKS_OK;
    if (!g_opened && mount_plain()) {
        g_plain = 1;
        g_opened = 1;
    }
    return g_opened;
}

/* The credentials sd-stub would have packed, as the cpio it would have made.
 *
 * The passphrase that just opened p3, because that is the one that opens p4
 * as well (DESIGN Section 5.1). The UEFI loader synthesises the same thing:
 * it publishes the UKI on a filesystem of its own with a drop-in directory
 * beside it holding the passphrase as a .cred file, and sd-stub turns that
 * into exactly this cpio. Here there is no sd-stub, so the cpio is written
 * directly and concatenated onto .initrd.
 *
 * And the install's identity, IDENT_CRED: the boot partition's record in its
 * text form, which the UEFI loader publishes beside every UKI and the
 * signing appliance opens its stick's partitions by (DESIGN Section 3.1).
 * Not a secret, and there on a plaintext shelf too.
 *
 * Returns its length, or 0 when there is neither to pack, which is not an
 * error.
 */
static size_t build_credential(void)
{
    cpio_entry e[4];
    size_t n = 2;
    size_t wrote = 0;

    if (g_pass_len == 0 && g_ident_len == 0)
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
    if (g_pass_len != 0) {
        e[n].path = CRED_PATH;
        e[n].data = g_pass;
        e[n].len = g_pass_len;
        /* 0600: it is a passphrase, and the initramfs it lands in is
         * root's. */
        e[n].mode = 0100600u;
        n++;
    }
    if (g_ident_len != 0) {
        e[n].path = IDENT_PATH;
        e[n].data = g_ident;
        e[n].len = g_ident_len;
        e[n].mode = 0100644u;
        n++;
    }

    if (cpio_write(e, n, g_credcpio, sizeof g_credcpio, &wrote) != CPIO_OK)
        return 0;
    return wrote;
}

void pos_set_install(const ident_rec *rec)
{
    g_ident_len = 0;
    if (rec != (const ident_rec *)0)
        g_ident_len = ident_encode_text(rec, g_ident, sizeof g_ident);
}

/* A linux_object over a section of an open file. */
typedef struct {
    fs_file *f;
    uint64_t base;
} sect_src;

static sect_src g_linux_src;
static sect_src g_initrd_src;

/* The UEFI loader's read says it is alive with a dot per 8 MiB, so a 60 MiB
 * image draws a short line rather than a pause that reads as a hang; the
 * sections are read here, so the dots are drawn here. */
#define POS_DOT_EVERY ((uint64_t)8u << 20)

static uint64_t g_done;
static uint64_t g_dot_at;

static int sect_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const sect_src *s = ctx;

    if (fs_file_read(s->f, s->base + off, len, buf) != FS_OK)
        return -1;
    g_done += len;
    while (g_done >= g_dot_at) {
        screen_out(".");
        g_dot_at += POS_DOT_EVERY;
    }
    return 0;
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

/* The moment every read is done and nothing of this loader will run again:
 * the lines the UEFI loader prints at the same point, then the wipe. */
static void handoff(void)
{
    screen_out("\n");
    screen_out_dec(g_read);
    screen_out(" bytes read.\n\n");
    screen_out("handing over to the image; everything below is its own "
               "output.\n\n");
    wipe_secrets();
}

/* Unwraps the chosen UKI and boots its kernel. Returns only on failure, with
 * r->stage and r->advice set. */
static int boot_uki(const fs_dirent *uki, boot_result *r)
{
    linux_object kernel;
    linux_object initrd[2] = { { 0 } };
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
    unsigned i;

    r->stage = "reading the UKI";
    /* The size comes from the open file, not from the directory entry: an
     * ext4 entry carries no size (falcon-core/src/fs/fs.c says so where it
     * builds one), and reading zero bytes and then asking whether they are a
     * PE answers "that is not a UKI" about every image on the shelf. */
    if (fs_file_open(&g_fs, &g_file, uki) != FS_OK)
        return -1;
    total = fs_file_size(&g_file);
    want = total < sizeof g_pehdr ? (size_t)total : sizeof g_pehdr;
    if (want == 0 || fs_file_read(&g_file, 0, want, g_pehdr) != FS_OK)
        return -1;
    if (pe_parse(g_pehdr, want, &hdr) != PE_OK) {
        r->advice = "the image is not a PE file, so it is not a UKI.";
        return -1;
    }
    if (pe_find_section(&hdr, g_pehdr, (size_t)total, ".linux",
                        &loff, &llen) != PE_OK || llen == 0) {
        r->advice = "the image has no .linux section, so it is not a UKI.";
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
        if (fs_file_read(&g_file, coff, clen, g_cmdline) != FS_OK)
            return -1;
        g_cmdline[clen] = '\0';
        /* The section is NUL-padded by ukify; stop at the first one. */
        for (i = 0; i < clen; i++) {
            if (g_cmdline[i] == '\0')
                break;
        }
        g_cmdline[i] = '\0';
    }

    credlen = build_credential();
    if (credlen > 0 && n_initrd < 2u) {
        g_credsrc.at = g_credcpio;
        g_credsrc.len = credlen;
        initrd[n_initrd].read = mem_read;
        initrd[n_initrd].ctx = &g_credsrc;
        initrd[n_initrd].size = credlen;
        n_initrd++;
        bios_puts(g_pass_len > 0 ? "falcon-bios: passphrase and identity "
                                    "credentials packed\n"
                                  : "falcon-bios: identity credential packed\n");
    } else if (g_pass_len > 0) {
        screen_out("warning: the passphrase credential could not be packed."
                   "\n         The initramfs will ask for the passphrase "
                   "again.\n\n");
    }

    g_read = kernel.size;
    for (i = 0; i < n_initrd; i++)
        g_read += initrd[i].size;
    g_done = 0;
    g_dot_at = POS_DOT_EVERY;

    r->stage = "starting the kernel";
    /* handoff as linux_boot's last act: the kernel, the initrd and the
     * credential cpio are all read through p3 or out of g_credcpio, so none
     * of it can go before they are copied, and after the jump nothing of
     * this loader runs to do it. The one copy that survives is the
     * credential inside the initrd, which is what it is for. */
    (void)linux_boot(&kernel, initrd, n_initrd, g_cmdline, handoff);
    boot_result_advise(r, linux_why(), (const char *)0);
    return -1;
}

/* An argon2id keyslot's memory: the heap, opened over the E820 map for the
 * one derivation and forgotten after it. Nothing else holds the heap while
 * a passphrase is being tried. */
static void *kdf_get(void *ctx, uint32_t kib)
{
    uint64_t n = (uint64_t)kib * 1024u;

    (void)ctx;
    heap_reset();
    if (!heap_open(n))
        return (void *)0;
    return heap_alloc(n);
}

static void kdf_put(void *ctx, void *mem, uint32_t kib)
{
    (void)ctx;
    (void)mem;
    (void)kib;
    heap_reset();
}

static const luks_kdf_mem g_kdf_mem = {
    kdf_get, kdf_put, (void *)0, (void *)0
};


/* Appends v in decimal at r->buf + *n, cut to fit like boot_result_advise. */
static void advise_dec(boot_result *r, size_t *n, uint64_t v)
{
    char d[21];
    size_t k = 0;

    do {
        d[k++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (k > 0u && *n + 2u < BOOT_ADVICE_MAX)
        r->buf[(*n)++] = d[--k];
}

static void advise_str(boot_result *r, size_t *n, const char *s)
{
    while (*s != '\0' && *n + 2u < BOOT_ADVICE_MAX)
        r->buf[(*n)++] = *s++;
}

/* The refusal for an argon2id keyslot the heap cannot hold, in the UEFI
 * loader's words (falcon-efi/src/pos/pos.c, kdf_advice). "Free" is the
 * largest run heap_open could have taken, which is the number the request
 * was measured against. */
static void kdf_advice(boot_result *r, uint32_t need_kib)
{
    size_t n = 0;

    advise_str(r, &n, "this keyslot needs ");
    advise_dec(r, &n, ((uint64_t)need_kib + 1023u) / 1024u);
    advise_str(r, &n, " MiB of memory; this machine has ");
    advise_dec(r, &n, heap_largest() >> 20);
    advise_str(r, &n, " MiB free.");
    r->buf[n] = '\0';
    r->advice = r->buf;
}

/* The advice for a shelf that would not mount, in the UEFI loader's words
 * (falcon-efi/src/pos/pos.c, mount_advice, which argues them). */
static void mount_advice(boot_result *r, int rc, const char *otherwise)
{
    size_t n = 0;

    if (rc == FS_ERR_JOURNAL) {
        r->advice = "the shelf's journal is damaged; run fsck on it from "
                    "another system, then boot its OS once with noresume.";
    } else if (rc == FS_ERR_NOMEM) {
        advise_str(r, &n, "reading the shelf's journal needs ");
        advise_dec(r, &n, ((uint64_t)g_jinfo.need + 0xFFFFFu) >> 20);
        advise_str(r, &n, " MiB of memory; this machine has ");
        advise_dec(r, &n, jnl_free() >> 20);
        advise_str(r, &n, " MiB free.");
        r->buf[n] = '\0';
        r->advice = r->buf;
    } else if (rc == FS_ERR_NEEDS_RECOVERY || rc == FS_ERR_DIRTY) {
        r->advice = "the filesystem was not cleanly unmounted; run fsck on it "
                    "from Linux.";
    } else {
        r->advice = otherwise;
    }
}

/* Workers worth starting for this shelf's keyslots: one fewer than the
 * most lanes any argon2id slot costly enough for them has (lanes_worth),
 * 0 when none is. */
static uint32_t lanes_wanted(void)
{
    size_t n = 0, k;
    uint32_t want = 0;

    if (luks_slot_count(&g_vol, &n) != LUKS_OK)
        return 0;
    for (k = 0; k < n; k++) {
        luks_slot_info si;

        if (luks_slot_at(&g_vol, k, &si) == LUKS_OK && si.usable &&
            si.kdf == LUKS_KDF_ARGON2ID &&
            lanes_worth(si.kdf_memory_kib, si.kdf_time, si.kdf_lanes) &&
            si.kdf_lanes - 1u > want)
            want = si.kdf_lanes - 1u;
    }
    return want;
}

/* Decimal, for a serial line. */
static void puts_dec(uint32_t v)
{
    char d[11];
    int n = 0;

    do {
        d[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n > 0) {
        char s[2] = { d[--n], 0 };

        bios_puts(s);
    }
}

/* The keyslots on every processor worth it (mp/mp.h), the AVX state
 * borrowed for them (xstate.h), both given back before anything else runs;
 * what ran and how long the keyslots took said on serial, the time by the
 * BIOS tick count, to the 55 ms it counts in. */
static int unlock_once(uint32_t *slot)
{
    luks_kdf_mem km = g_kdf_mem;
    xstate_saved xs;
    uint32_t t0, procs;
    int rc;

    xstate_borrow(&xs);
    bios_puts("falcon-bios: argon2id block function: ");
    bios_puts(crypto_simd_name(crypto_simd_level()));
    bios_puts("\n");
    km.run = mp_start(crypto_simd_level(), lanes_wanted());
    t0 = peek32(0x46Cu);
    rc = luks_unlock(&g_vol, g_pass, g_pass_len, &km, &g_mk, slot);
    t0 = peek32(0x46Cu) - t0;
    procs = mp_count();
    mp_stop();
    xstate_return(&xs);
    bios_puts("falcon-bios: argon2id ran on ");
    puts_dec(procs);
    bios_puts(" processors in ");
    puts_dec(t0 * 10000u / 182u);
    bios_puts(" ms\n");
    return rc;
}

/* Prompt, retry, and report, as the UEFI loader does. LUKS_OK, POS_ABORTED
 * for escape, or the LUKS status to report. */
static int prompt_and_unlock(void)
{
    for (;;) {
        uint32_t slot = 0;
        int rc;
        unsigned wait;

        screen_out("passphrase for the encrypted partition: ");

        if (prompt_passphrase(g_pass, sizeof g_pass, &g_pass_len) != 0)
            return POS_ABORTED;

        screen_out("unlocking...\n");
        rc = unlock_once(&slot);
        if (rc == LUKS_OK)
            return LUKS_OK;
        if (rc != LUKS_ERR_NOKEY)
            return rc;      /* not something a retype fixes */
        crypto_wipe(g_pass, sizeof g_pass);
        g_pass_len = 0;
        g_failures++;
        /* Not said, as the UEFI loader does not say it
         * (falcon-efi/src/pos/pos.c): the wait runs under "unlocking...". */
        if (g_failures >= TUI_UNLOCK_TRIES) {
            screen_out("wrong passphrase.\n");
            screen_pause(3);
            screen_power_off();
        }
        wait = tui_unlock_delay(g_failures);
        screen_pause(wait);
        screen_out("wrong passphrase.\n\n");
    }
}

/* Two or more images are offered; one is booted without asking. */
static int choose_uki(void)
{
    static const screen_banner banner = {
        u"falcon-boot: which image?", (const uint16_t *)0, 0
    };
    size_t i;

    if (g_images.n == 1)
        return 0;
    shelf_label(&g_images, &g_fs);
    for (i = 0; i < g_images.n; i++)
        g_rows[i] = g_images.uki[i].label;
    return screen_menu(&banner, g_rows, g_images.n, (const char *)0,
                       tui_hint_plain, (int *)0);
}

const uint16_t *pos_entry_label(void)
{
    /* The UEFI loader's labels (falcon-efi/src/pos/pos.c). */
    if (g_plain)
        return u"Personal OS (plaintext shelf)";
    return u"" STROWS_LABEL_POS;
}

/* Unlocks the encrypted shelf and mounts what is inside it into g_fs.
 * LUKS_OK, or POS_ABORTED or -1 with *r filled in; secrets wiped on every
 * failure. */
static int open_encrypted(boot_result *r)
{
    const fs_ops *ops;
    int rc;

    r->stage = "unlocking the encrypted partition";
    rc = prompt_and_unlock();
    if (rc != LUKS_OK) {
        wipe_secrets();
        if (rc == POS_ABORTED) {
            r->stage = "passphrase prompt";
            return POS_ABORTED;
        }
        if (rc == LUKS_ERR_NOKEY)
            r->advice = "the passphrase did not open any keyslot.";
        else if (rc == LUKS_ERR_IO)
            r->advice = "the partition could not be read.";
        else if (rc == LUKS_ERR_KDF_MEMORY)
            kdf_advice(r, g_vol.kdf_need_kib);
        else
            r->advice = "the volume is outside what this loader supports.";
        return -1;
    }

    r->stage = "opening the decrypted volume";
    if (luks_dev_open(&g_dev, &g_vol, &g_mk) != LUKS_OK) {
        wipe_secrets();
        return -1;
    }
    g_dev_open = 1;
    /* luks_dev_open has taken the key into its cipher context, so the copy
     * here is dead from this point on. */
    crypto_wipe(&g_mk, sizeof g_mk);

    r->stage = "mounting the filesystem inside it";
    ops = fs_identify(luks_read, &g_dev, FS_HINT_SHELF);
    rc = ops == (const fs_ops *)0
             ? FS_ERR_BAD_SUPER
             : fs_mount_journal(&g_fs, ops, luks_read, &g_dev, &jnl_mem,
                                &g_jinfo);
    if (rc != FS_OK) {
        mount_advice(r, rc, "the decrypted volume is not a filesystem this "
                            "loader can read.");
        wipe_secrets();
        return -1;
    }

    return LUKS_OK;
}

int pos_list(bios_disk *d, const part_entry *shelf, pos_list_fn fn,
             void *ctx)
{
    size_t i;
    int n;

    if (!pos_probe(d, shelf) || !g_plain)
        return -1;
    if (shelf_collect(&g_images, &g_fs) != 1) {
        shelf_wipe(&g_images);
        return 0;
    }
    shelf_label(&g_images, &g_fs);
    for (i = 0; i < g_images.n; i++)
        fn(&g_images.uki[i], ctx);
    n = (int)g_images.n;
    shelf_wipe(&g_images);
    return n;
}

int pos_run_pick(bios_disk *d, const part_entry *shelf, pos_pick_fn want,
                 void *ctx, boot_result *r)
{
    const fs_dirent *uki;
    int pick;
    int rc;

    boot_result_init(r);
    if (!pos_probe(d, shelf)) {
        r->stage = "looking for a UKI shelf";
        return -1;
    }
    if (!g_plain) {
        rc = open_encrypted(r);
        if (rc != LUKS_OK)
            return rc;
    }

    r->stage = "looking for a UKI";
    if (shelf_collect(&g_images, &g_fs) != 1) {
        r->advice = "no *.efi image was found in /uki or /EFI/Linux.";
        wipe_secrets();
        return -1;
    }

    if (want != (pos_pick_fn)0) {
        size_t i;

        pick = -1;
        for (i = 0; i < g_images.n && pick < 0; i++) {
            if (want(&g_images.uki[i].ent, ctx))
                pick = (int)i;
        }
        if (pick < 0) {
            r->advice = "the image this entry names is not on the shelf.";
            wipe_secrets();
            return -1;
        }
    } else {
        pick = choose_uki();
        if (pick < 0) {
            r->stage = "choosing an image";
            wipe_secrets();
            return POS_ABORTED;
        }
    }
    uki = &g_images.uki[pick].ent;

    screen_page();
    /* As the UEFI loader says it (falcon-efi/src/pos/pos.c). */
    if ((g_jinfo.flags & FS_JNL_FC_SKIPPED) != 0)
        screen_out("the shelf's last few seconds of changes were not read.\n");
    if ((g_jinfo.flags & FS_JNL_REPLAYED) != 0)
        bios_puts("falcon-bios: the shelf is read through its journal\n");
    screen_out("reading ");
    screen_out16(uki->name);
    screen_out(" ");
    rc = boot_uki(uki, r);
    wipe_secrets();
    return rc;
}

int pos_run(bios_disk *d, const part_entry *shelf, boot_result *r)
{
    return pos_run_pick(d, shelf, (pos_pick_fn)0, (void *)0, r);
}
