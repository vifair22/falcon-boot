/* The personal-OS path: passphrase -> p3 -> filesystem -> UKI -> credfs ->
 * shim, with the passphrase step skipped on a plaintext shelf.
 * See pos.h for the contract and, in particular, for why the UKI is loaded by
 * device path off a filesystem this loader synthesizes.
 */

#include <pos/pos.h>
#include <media/osrel.h>
#include <media/strows.h>

#include <loader/loadops.h>
#include <loader/bli.h>

#include <loader/credfs.h>
#include <crypto/crypto.h>
#include <crypto/xstate.h>
#include <console/serial.h>
#include <mp/mp.h>
#include <tsc/tsc.h>
#include <tsc/tsccore.h>
#include <discover/discover.h>
#include <fs/fs.h>
#include <mem/jnlmem.h>
#include <ident/ident.h>
#include <luks/luks.h>
#include <lib/string.h>
#include <lib/ucs2.h>
#include <manifest/blesslist.h>
#include <console/menu.h>
#include <console/idle.h>
#include <media/shelf.h>
#include <tui/tui.h>

/* The shelves carrying this install's ID. A stick has one (p3); the room is
 * for an install that was given a second, which nothing makes today. */
#define POS_MAX_VOLS 4u

/* 256 bytes of passphrase plus the terminator. The limit is the prompt's, not
 * the format's; LUKS itself has no such cap. */
#define POS_PASS_MAX 257u

/* Wrong passphrases this boot, across every prompt: TUI_UNLOCK_TRIES of
 * them powers the machine off, and each of the others costs a wait
 * (tui_unlock_delay). An attacker holding the disk grinds the header
 * offline, where argon2id's cost is the defence; this is the defence
 * against one at the keyboard of a machine that stays whole, the station's
 * case. Returning to the menu does not start the count again. */
static unsigned g_failures;

/* Read granularity for the UKI, and how often the read says it is alive. A
 * 1 MiB request costs one extent-tree descent and a run of block reads; the
 * dot is per 8 MiB so a 60 MiB UKI draws a short line rather than a wall. */
#define POS_READ_CHUNK ((uint64_t)1u << 20)
#define POS_DOT_EVERY  ((uint64_t)8u << 20)

/* A sanity bound, not a format limit: a UKI is tens of megabytes, and a
 * plausible-looking size in the gigabytes is a corrupt inode rather than a
 * kernel. Allocating on it before finding that out is how a bad p3 turns into
 * a hang. */
#define POS_UKI_MAX ((uint64_t)512u << 20)

/* Room for the longest name a filesystem here can hold, as CHAR16. The
 * synthetic filesystem reuses the UKI's own name, so an operator reading a
 * failure line sees the file they picked rather than a placeholder. */
#define POS_NAME_MAX 256u

static EFI_SYSTEM_TABLE *g_st;

/* The station's TOTP lines (pos_set_station_hooks); NULL on the stick. */
static void (*g_note)(void);
static void (*g_handover)(void);

/* ---- console ---- */

/* Inside the content block, like every other screen; see
 * falcon-efi/src/console/menu.h. */
static void out(const CHAR16 *s)
{
    menu_out(g_st, s);
}

static void out_dec(UINT64 v)
{
    CHAR16 buf[21];
    UINTN i = sizeof(buf) / sizeof(buf[0]) - 1;

    buf[i] = L'\0';
    do {
        buf[--i] = (CHAR16)(L'0' + (v % 10));
        v /= 10;
    } while (v != 0);
    out(&buf[i]);
}

/* Directory-entry names arrive as UCS-2 from the registry, whatever the
 * filesystem underneath stores (falcon-core/src/fs widens a byte-named driver's
 * names on the way out). Everything the loader will find on p3 is ASCII by
 * construction (mkstick writes those names), so anything else is shown as '?'
 * rather than guessed at: a byte-widened UTF-8 name is not text, and a console
 * line is the wrong place to find that out. */
static void name_to_ucs2(CHAR16 *dst, UINTN cap, const uint16_t *name,
                         UINTN len)
{
    UINTN i;

    for (i = 0; i < len && i + 1 < cap; i++)
        dst[i] = (name[i] >= 0x20 && name[i] < 0x7F) ? (CHAR16)name[i]
                                                     : L'?';
    dst[i] = 0;
}

static void out_name(const uint16_t *name, UINTN len)
{
    CHAR16 buf[64];

    name_to_ucs2(buf, sizeof(buf) / sizeof(buf[0]), name, len);
    out(buf);
}

/* ---- shelf volumes found at scan time ----
 *
 * Two shapes, because DESIGN Section 3 makes p3's encryption the default
 * rather than a requirement: a LUKS2 volume this loader can unlock, and a
 * plaintext shelf carrying a filesystem the registry claimed. Everything
 * downstream of the mount is identical; the difference is one prompt and one
 * credential, and it is confined to pos_boot's opening moves. */
typedef struct {
    discover_dev *dev;
    const fs_ops *fs;        /* plaintext only: the driver that claimed it */
    uint8_t       encrypted; /* a LUKS2 header sits at offset 0 */
    uint8_t       usable;    /* encrypted: a keyslot we can process;
                                plaintext: a mountable volume with a UKI on
                                it, checked at scan time because here, unlike
                                the encrypted case, we CAN check */
    luks_vol      vol;
    /* luks_open keeps views into this for the lifetime of vol, so it is
     * per-volume storage and not one shared scratch buffer. */
    char          json[LUKS_JSON_CAP];
} pos_shelf;

static pos_shelf g_shelf[POS_MAX_VOLS];
static UINTN     g_shelf_n;
static UINTN     g_luks_usable;
static UINTN     g_plain_usable;

/* The secret, and the key it recovers. Both are file statics rather than
 * locals so that "everything that ever held key material" is two objects with
 * one wipe site each, rather than a set of stack frames nobody can audit. */
static char            g_pass[POS_PASS_MAX];
static UINTN           g_pass_len;
static luks_master_key g_mk;

/* The copy that travels. Separate from g_pass so the buffer the user typed
 * into can die as early as it always did (right after the unlock) while the
 * secret still reaches the initramfs, and so that "the copy credfs serves"
 * is one named object with one owner rather than an aliased pointer. credfs
 * wipes it at teardown; wipe_secrets is the belt for every path that never
 * got as far as publishing one. */
static char            g_cred[POS_PASS_MAX];
static UINTN           g_cred_len;

static void wipe_secrets(void)
{
    crypto_wipe(g_pass, sizeof g_pass);
    crypto_wipe(g_cred, sizeof g_cred);
    crypto_wipe(&g_mk, sizeof g_mk);
    g_pass_len = 0;
    g_cred_len = 0;
}

/* Decrypted-volume and filesystem state. Static because each carries a
 * multi-kilobyte cache and -Wstack-usage polices frames at 32 KB. */
static luks_dev  g_dev;
static uint8_t   g_dev_open;   /* g_dev holds a cipher context to close */
static fs_vol    g_fs;
static fs_file   g_file;

/* The images on the shelf, their chooser labels, and the directory and
 * section caches that found them (falcon-core/src/media/shelf.h). */
static shelf_list g_images;

/* The filesystem caches hold plaintext read out of an encrypted volume. It
 * is not key material, so nothing turns on it, but leaving fragments of p3
 * sitting in memory for whatever boots next is not a thing to do by
 * omission. The LUKS device holds the volume key's schedule, which is.
 * crypto_wipe rather than memset because these are exactly the dead-buffer
 * stores an optimiser is entitled to drop. Safe to call twice. */
static void wipe_shelf(void)
{
    crypto_wipe(&g_fs, sizeof g_fs);
    jnl_release();
    shelf_wipe(&g_images);
    crypto_wipe(&g_file, sizeof g_file);
    if (g_dev_open) {
        luks_dev_close(&g_dev);
        g_dev_open = 0;
    }
}

/* The chooser's rows, filled from g_images' labels. */
static menu_entry g_uki_entries[SHELF_MAX];

/* Does this volume have a keyslot this loader can use: pbkdf2 or argon2id?
 * An argon2i slot, or a KDF the LUKS module does not know, is recognised
 * and reported, never misread; a volume holding only those is not offered.
 * Whether an argon2id slot's memory can be had is found out at the unlock,
 * where the refusal can say how much it asked for. */
static uint8_t has_usable_slot(const luks_vol *v)
{
    size_t count = 0;
    size_t i;

    if (luks_slot_count(v, &count) != LUKS_OK)
        return 0;
    for (i = 0; i < count; i++) {
        luks_slot_info info;

        if (luks_slot_at(v, i, &info) != LUKS_OK)
            continue;
        if (info.usable)
            return 1;
    }
    return 0;
}

/* Can this plaintext shelf actually serve a personal OS? Forward-declared
 * because the answer is "mount it and look for a UKI", and the code that
 * knows how to do that sits below with the rest of the filesystem walk. */
static int plain_shelf_usable(discover_dev *dev, const fs_ops *ops);

/* The shelf-role rows of the inventory: this install's shelves, by record
 * (discover.h). Appends to g_shelf. */
static void scan_shelves(void)
{
    UINTN cursor = 0;
    discover_dev *dev;

    /* The rows are falcon-efi/src/discover/discover.c's, classified once for
     * the whole loader (DESIGN Section 9), and only those whose record
     * carries this loader's install ID come back (Section 3.1). The LUKS2
     * magic on each row decides which of the two shapes below this one is. */
    while (g_shelf_n < POS_MAX_VOLS &&
           (dev = discover_next(IDENT_ROLE_SHELF, &cursor)) !=
               (discover_dev *)0) {
        pos_shelf *p = &g_shelf[g_shelf_n];

        p->dev = dev;
        p->fs = (const fs_ops *)0;
        p->encrypted = dev->luks2;
        p->usable = 0;

        if (dev->luks2) {
            if (luks_open(&p->vol, discover_read, dev, p->json,
                          LUKS_JSON_CAP) != LUKS_OK)
                continue;
            p->usable = has_usable_slot(&p->vol);
            if (p->usable)
                g_luks_usable++;
            g_shelf_n++;
            continue;
        }

        /* A plaintext shelf (DESIGN Section 3's --p3-plain), offered only
         * when there is something on it to boot: its contents are readable
         * now, so an empty one is found out here rather than after the
         * entry is picked. */
        if (dev->fs == (const fs_ops *)0)
            continue;
        if (!plain_shelf_usable(dev, dev->fs))
            continue;
        p->fs = dev->fs;
        p->usable = 1;
        g_plain_usable++;
        g_shelf_n++;
    }
}

UINTN pos_scan(EFI_SYSTEM_TABLE *st)
{
    g_st = st;
    g_shelf_n = 0;
    g_luks_usable = 0;
    g_plain_usable = 0;

    /* unlock_any tries one passphrase against every recorded shelf, which
     * is sound because every one of them carries this install's ID: a
     * second stick's shelf, or the machine's own, is never in the list. */
    scan_shelves();

    return g_luks_usable + g_plain_usable;
}

/* ---- unlock ---- */

/* One passphrase against every unlockable volume.
 *
 * The same buffer is tried against each. DESIGN Section 3 gives p3 and p4 one
 * passphrase, and re-prompting per volume would make the user type the same
 * string twice to find out which partition the firmware happened to enumerate
 * first. Volumes with no usable keyslot were already dropped by the scan, so
 * they are neither prompted for nor attempted.
 *
 * *which receives the index that opened. The distinction the caller needs is
 * "nobody accepted it", which is worth retrying, against "something broke",
 * which is not; so a NOKEY anywhere outranks a device error elsewhere,
 * because a mistyped passphrase is by far the likelier explanation and is the
 * one the user can act on. */
/* An argon2id keyslot's memory, from the firmware as pages. One block is
 * out at a time (luks.h), so the page count is kept here rather than asked
 * of the caller again. */
static EFI_PHYSICAL_ADDRESS g_kdf_addr;
static UINTN                g_kdf_pages;

/* argon2id's block, 2 MiB aligned inside an allocation 2 MiB larger.
 * Its every access is a random one across the whole block, so it is paced
 * by the TLB: a firmware maps memory with 2 MiB pages, and splits a range
 * into 4 KiB ones only where an attribute change starts or ends inside
 * one (edk2 marks data no-execute that way), so a block on 2 MiB
 * boundaries keeps its large pages whatever happens at the allocation's
 * ends. */
#define KDF_ALIGN ((uint64_t)2u << 20)

static void *kdf_get(void *ctx, uint32_t kib)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    UINTN pages = (UINTN)(((uint64_t)kib * 1024u + KDF_ALIGN +
                           EFI_PAGE_SIZE - 1u) / EFI_PAGE_SIZE);

    (void)ctx;
    g_kdf_addr = 0;
    g_kdf_pages = 0;
    if (kib == 0 ||
        bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages,
                          &g_kdf_addr) != EFI_SUCCESS)
        return (void *)0;
    g_kdf_pages = pages;
    return (void *)(uintptr_t)((g_kdf_addr + KDF_ALIGN - 1u) &
                               ~(KDF_ALIGN - 1u));
}

static void kdf_put(void *ctx, void *mem, uint32_t kib)
{
    (void)ctx;
    (void)mem;
    (void)kib;
    if (g_kdf_pages != 0)
        (void)g_st->BootServices->FreePages(g_kdf_addr, g_kdf_pages);
    g_kdf_pages = 0;
}

static const luks_kdf_mem g_kdf_mem = {
    kdf_get, kdf_put, (void *)0, (void *)0
};

/* The smallest ask of an argon2id slot the firmware could not supply, from
 * the last unlock_any that returned LUKS_ERR_KDF_MEMORY. */
static uint32_t g_kdf_need_kib;

/* The largest run of free conventional memory, in bytes: the most one
 * AllocatePages call could have returned. Adjacent runs are joined, since a
 * firmware need not merge them in its map. 0 when the map cannot be read. */
static UINT64 largest_free(void)
{
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    EFI_MEMORY_DESCRIPTOR *map = (EFI_MEMORY_DESCRIPTOR *)0;
    UINTN size = 0;
    UINTN key = 0;
    UINTN dsize = 0;
    UINT32 dver = 0;
    UINT64 best = 0;
    UINT64 run_start = 0;
    UINT64 run_end = 0;
    UINTN off;
    EFI_STATUS st;

    st = bs->GetMemoryMap(&size, map, &key, &dsize, &dver);
    if (st != EFI_BUFFER_TOO_SMALL || dsize < sizeof(EFI_MEMORY_DESCRIPTOR))
        return 0;
    /* The pool allocation below can split an entry into three. */
    size += 4u * dsize;
    if (bs->AllocatePool(EfiLoaderData, size, (void **)&map) != EFI_SUCCESS)
        return 0;
    st = bs->GetMemoryMap(&size, map, &key, &dsize, &dver);
    if (st != EFI_SUCCESS) {
        (void)bs->FreePool(map);
        return 0;
    }
    for (off = 0; off + dsize <= size; off += dsize) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(const void *)((UINT8 *)map + off);
        UINT64 len = d->NumberOfPages * EFI_PAGE_SIZE;

        if (d->Type != EfiConventionalMemory)
            continue;
        if (d->PhysicalStart == run_end && run_end != 0) {
            run_end += len;
        } else {
            run_start = d->PhysicalStart;
            run_end = run_start + len;
        }
        if (run_end - run_start > best)
            best = run_end - run_start;
    }
    (void)bs->FreePool(map);
    return best;
}

static void advice_str(CHAR16 *buf, UINTN cap, UINTN *n, const char *s)
{
    while (*s != '\0' && *n + 1u < cap)
        buf[(*n)++] = (CHAR16)*s++;
}

static void advice_dec(CHAR16 *buf, UINTN cap, UINTN *n, UINT64 v)
{
    char d[21];
    UINTN k = 0;

    do {
        d[k++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (k > 0u && *n + 1u < cap)
        buf[(*n)++] = (CHAR16)d[--k];
}

/* The refusal for an argon2id keyslot the firmware cannot give memory for,
 * in falcon-bios's words (falcon-bios/src/pos/pos.c, kdf_advice). */
static const CHAR16 *kdf_advice(void)
{
    static CHAR16 buf[96];
    UINTN n = 0;
    const UINTN cap = sizeof(buf) / sizeof(buf[0]);

    advice_str(buf, cap, &n, "this keyslot needs ");
    advice_dec(buf, cap, &n, ((UINT64)g_kdf_need_kib + 1023u) / 1024u);
    advice_str(buf, cap, &n, " MiB of memory; this machine has ");
    advice_dec(buf, cap, &n, largest_free() >> 20);
    advice_str(buf, cap, &n, " MiB free.");
    buf[n] = 0;
    return buf;
}

/* Into buf at *at, which stays at the terminator, as much as fits. */
static void put_str(char *buf, UINTN cap, UINTN *at, const char *s)
{
    while (*s != '\0' && *at + 1u < cap)
        buf[(*at)++] = *s++;
    buf[*at] = '\0';
}

static void put_dec(char *buf, UINTN cap, UINTN *at, UINT64 v)
{
    char d[21];
    UINTN n = 0;

    do {
        d[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n > 0u && *at + 1u < cap)
        buf[(*at)++] = d[--n];
    buf[*at] = '\0';
}

/* head, then name, as one line on serial. */
static void serial_note(const char *head, const char *name)
{
    char line[96];
    UINTN n = 0;

    while (*head != '\0' && n + 1u < sizeof line)
        line[n++] = *head++;
    while (*name != '\0' && n + 1u < sizeof line)
        line[n++] = *name++;
    line[n] = '\0';
    serial_line(g_st, line);
}

/* The other processors serving argon2id lanes for an unlock (mp/mp.h).
 * Static because the application processors read it while unlock_any
 * runs. */
static mp_lanes g_mp;

/* Whether any keyslot an unlock may try costs enough to run its lanes on
 * the other processors (lanes_worth): a cheap one, the stick's, finishes
 * before the firmware would see the workers back. */
static int pool_worth(void)
{
    UINTN i;

    for (i = 0; i < g_shelf_n; i++) {
        size_t n = 0, k;

        if (!g_shelf[i].encrypted || !g_shelf[i].usable ||
            luks_slot_count(&g_shelf[i].vol, &n) != LUKS_OK)
            continue;
        for (k = 0; k < n; k++) {
            luks_slot_info si;

            if (luks_slot_at(&g_shelf[i].vol, k, &si) == LUKS_OK &&
                si.usable && si.kdf == LUKS_KDF_ARGON2ID &&
                lanes_worth(si.kdf_memory_kib, si.kdf_time, si.kdf_lanes))
                return 1;
        }
    }
    return 0;
}

static int unlock_any(UINTN *which)
{
    int other_err = LUKS_OK;
    int saw_nokey = 0;
    int saw_mem = 0;
    int rc = LUKS_ERR_NOKEY;
    xstate_saved xs;
    luks_kdf_mem km = g_kdf_mem;
    char num[48];
    tsc_hz_how how;
    uint32_t procs;
    UINT64 t0, ms;
    UINTN i, k;

    *which = 0;
    /* The AVX state for argon2id's block function, borrowed for the
     * derivations and given back before anything else runs (xstate.h);
     * then the other processors, which must run the level it gives. */
    xstate_borrow(&xs);
    serial_note("falcon-pos: argon2id block function: ",
                crypto_simd_name(crypto_simd_level()));
    g_mp.started = 0;
    if (pool_worth())
        km.run = mp_lanes_start(g_st->BootServices, &g_mp,
                                crypto_simd_level());
    t0 = tsc_read();
    for (i = 0; i < g_shelf_n; i++) {
        uint32_t slot = 0;

        if (!g_shelf[i].encrypted || !g_shelf[i].usable)
            continue;
        rc = luks_unlock(&g_shelf[i].vol, g_pass, (size_t)g_pass_len, &km,
                         &g_mk, &slot);
        if (rc == LUKS_OK) {
            *which = i;
            break;
        }
        if (rc == LUKS_ERR_NOKEY) {
            saw_nokey = 1;
        } else if (rc == LUKS_ERR_KDF_MEMORY) {
            /* Outranks NOKEY, as it does inside one volume (luks.h): the
             * passphrase may be that slot's, and a retype cannot tell. */
            if (!saw_mem || g_shelf[i].vol.kdf_need_kib < g_kdf_need_kib)
                g_kdf_need_kib = g_shelf[i].vol.kdf_need_kib;
            saw_mem = 1;
        } else if (other_err == LUKS_OK) {
            other_err = rc;
        }
    }

    ms = tsc_usec(tsc_read() - t0, tsc_hz(g_st->BootServices, &how)) / 1000u;
    procs = mp_lanes_count(&g_mp);
    mp_lanes_stop(g_st->BootServices, &g_mp);
    xstate_return(&xs);
    /* The keyslots' time without the release of the workers, which is the
     * firmware's, so the figure compares with mkstick kdf-bench's. */
    k = 0;
    put_dec(num, sizeof num, &k, procs);
    put_str(num, sizeof num, &k, " processors in ");
    put_dec(num, sizeof num, &k, ms);
    put_str(num, sizeof num, &k, " ms");
    serial_note("falcon-pos: argon2id ran on ", num);

    if (rc == LUKS_OK)
        return LUKS_OK;
    if (saw_mem)
        return LUKS_ERR_KDF_MEMORY;
    if (saw_nokey)
        return LUKS_ERR_NOKEY;
    return (other_err != LUKS_OK) ? other_err : LUKS_ERR_NOKEY;
}

/* What the last shelf mount did (fs_mount_journal), for the booting page and
 * a refusal's advice. The table itself is mem/jnlmem.c's, released with the
 * rest of the shelf's state (wipe_shelf) or at the next mount. */
static fs_jinfo g_jinfo;

/* Mount the shelf's filesystem through the registry rather than by naming a
 * driver (DESIGN Section 9). `known` is the identification
 * falcon-efi/src/discover/discover.c already made for a plaintext volume; the
 * decrypted case has none, because nothing could read those bytes until a
 * moment ago, so it identifies now. */
static int mount_shelf(fs_read_fn read, void *ctx, const fs_ops *known)
{
    const fs_ops *ops = known;
    fs_jmem mem;

    if (ops == (const fs_ops *)0)
        ops = fs_identify(read, ctx, FS_HINT_SHELF);
    if (ops == (const fs_ops *)0)
        return FS_ERR_BAD_SUPER;
    jnl_mem_make(&mem, g_st->BootServices);
    return fs_mount_journal(&g_fs, ops, read, ctx, &mem, &g_jinfo);
}

/* The advice for a shelf that would not mount. A shelf its OS left mounted
 * (hibernated, or the power went) is read through its journal and mounts;
 * what is left is a journal that is damaged, or one this machine cannot give
 * the memory to read, or errors the filesystem itself recorded. fsck is the
 * answer to the first and the last, and it is only safe once nothing still
 * counts on the shelf as it was: a hibernated OS resumed over a repaired
 * shelf corrupts it, hence noresume. `otherwise` is for a format problem.
 * falcon-bios/src/pos/pos.c says the same sentences. */
static const CHAR16 *mount_advice(int rc, const CHAR16 *otherwise)
{
    static CHAR16 buf[112];
    UINTN n = 0;
    const UINTN cap = sizeof(buf) / sizeof(buf[0]);

    if (rc == FS_ERR_JOURNAL)
        return L"the shelf's journal is damaged; run fsck on it from another "
               L"system, then boot its OS once with noresume.";
    if (rc == FS_ERR_NOMEM) {
        advice_str(buf, cap, &n, "reading the shelf's journal needs ");
        advice_dec(buf, cap, &n, ((UINT64)g_jinfo.need + 0xFFFFFu) >> 20);
        advice_str(buf, cap, &n, " MiB of memory; this machine has ");
        advice_dec(buf, cap, &n, largest_free() >> 20);
        advice_str(buf, cap, &n, " MiB free.");
        buf[n] = 0;
        return buf;
    }
    if (rc == FS_ERR_NEEDS_RECOVERY || rc == FS_ERR_DIRTY)
        return L"the filesystem was not cleanly unmounted; run fsck on it "
               L"from Linux.";
    return otherwise;
}

/* Scan-time question for a plaintext shelf: is there a UKI on it?
 *
 * Mounts, looks, and leaves nothing behind. The encrypted case cannot be
 * asked this (its contents are unreadable until the user types the
 * passphrase) and is offered on the header alone; here the answer is cheap
 * and refusing to ask would put a Personal OS entry in the menu for any ext4
 * volume that happens to be plugged in. */
static int plain_shelf_usable(discover_dev *dev, const fs_ops *ops)
{
    int ok = 0;

    if (mount_shelf(discover_read, dev, ops) == FS_OK)
        ok = (shelf_collect(&g_images, &g_fs) == 1);

    /* A look, not a claim: pos_boot mounts for itself when the entry is
     * picked, so nothing here stays live, and the caches go the same way the
     * decrypted ones do. */
    crypto_wipe(&g_fs, sizeof g_fs);
    shelf_wipe(&g_images);
    return ok;
}

/* Which UKI to boot. One is booted without asking; several are offered,
 * because on a stick built to DESIGN Section 7.5 the second one is the
 * signing appliance and picking for the user would be picking wrong half the
 * time. Returns the index, or a negative value if the user backed out.
 *
 * The appliance case asks nothing at all: the user has already said what they
 * want (DESIGN Section 6.3.1's one confirmation), and a chooser here would
 * only be a second place to pick the wrong image. A shelf with no appliance on
 * it is a negative return, which the caller turns into the message that says
 * so. */
/* Does a directory entry's name say `file`, ASCII-case-insensitively? */
static BOOLEAN name_is(const fs_dirent *e, const char *file)
{
    UINTN i;

    for (i = 0; i < e->name_len; i++) {
        uint16_t a = e->name[i];
        char b = file[i];

        if (b == '\0')
            return FALSE;
        if (a >= 'A' && a <= 'Z')
            a = (uint16_t)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != (uint16_t)(unsigned char)b)
            return FALSE;
    }
    return (BOOLEAN)(file[i] == '\0');
}

static INTN choose_uki(const menu_banner *banner, pos_pick pick,
                       const char *file)
{
    menu_keys keys;
    UINTN i;

    /* Named by the caller (a station row, its default, next boot): that
     * file or nothing, never the chooser in its place. */
    if (file != (const char *)0) {
        for (i = 0; i < g_images.n; i++) {
            if (name_is(&g_images.uki[i].ent, file))
                return (INTN)i;
        }
        return -2;
    }

    if (pick == POS_PICK_APPLIANCE) {
        for (i = 0; i < g_images.n; i++) {
            if (shelf_is_appliance(&g_images.uki[i].ent))
                return (INTN)i;
        }
        return -1;
    }

    if (g_images.n == 1)
        return 0;
    keys.keys = (const CHAR16 *)0;
    keys.hint = (const CHAR16 *)tui_hint_plain;

    /* The rows' names, order and qualifiers are falcon-core's
     * (media/shelf.c), so falcon-bios offers the same rows. */
    shelf_label(&g_images, &g_fs);
    for (i = 0; i < g_images.n; i++) {
        ucs2_copy(g_uki_entries[i].label, MENU_PATH_MAX,
                  g_images.uki[i].label);
        g_uki_entries[i].sig[0] = 0;
        g_uki_entries[i].why[0] = 0;
        g_uki_entries[i].caution = 0;
    }
    /* No row here carries a trust tag, so the key line has no legend. */
    return menu_run_keys(g_st, banner, g_uki_entries, g_images.n, &keys,
                         (CHAR16 *)0);
}

/* ---- reading the UKI ---- */

/* Reads the whole UKI into freshly allocated pages.
 *
 * Whole, not streamed: LoadImage takes a buffer and a length, so the image
 * has to be contiguous in memory before the call. Pages rather than pool
 * because the buffer is tens of megabytes and page-aligned is what every PE
 * loader wants to start from. */
static EFI_STATUS read_uki(EFI_BOOT_SERVICES *bs, const fs_dirent *ent,
                           void **buf_out, UINT64 *size_out, UINTN *pages_out)
{
    EFI_PHYSICAL_ADDRESS addr = 0;
    UINT64 size;
    UINT64 done = 0;
    UINT64 next_dot = POS_DOT_EVERY;
    UINTN pages;
    UINT8 *buf;
    EFI_STATUS status;

    *buf_out = (void *)0;
    *size_out = 0;
    *pages_out = 0;

    if (fs_file_open(&g_fs, &g_file, ent) != FS_OK)
        return EFI_DEVICE_ERROR;
    size = fs_file_size(&g_file);
    if (size == 0 || size > POS_UKI_MAX)
        return EFI_UNSUPPORTED;

    pages = (UINTN)((size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(status))
        return status;
    buf = (UINT8 *)(uintptr_t)addr;

    while (done < size) {
        UINT64 n = size - done;

        if (n > POS_READ_CHUNK)
            n = POS_READ_CHUNK;
        if (fs_file_read(&g_file, done, n, buf + done) != FS_OK) {
            bs->FreePages(addr, pages);
            return EFI_DEVICE_ERROR;
        }
        done += n;
        if (done >= next_dot) {
            out(L".");
            next_dot += POS_DOT_EVERY;
        }
    }

    *buf_out = buf;
    *size_out = size;
    *pages_out = pages;
    return EFI_SUCCESS;
}

/* ---- the synthetic credential filesystem (DESIGN Section 5.1) ---- */

/* The credential's filename, which is not a choice.
 *
 * Four links, each one fixed by systemd (verified against 261.2 sources) or
 * by the drop-in the third one needs:
 *
 *   1. sd-stub collects `<image>.efi.extra.d/<name>.cred` and packs them into
 *      a cpio it merges into the initrd at /.extra/credentials/
 *      (systemd-stub(7), COMPANION FILES).
 *   2. PID 1 imports that directory and DROPS the `.cred` suffix to get the
 *      credential name (src/core/import-creds.c, import_credentials_boot).
 *   3. every systemd-cryptsetup@ unit a generator writes carries
 *      `ImportCredential=cryptsetup.*` (src/shared/generator.c,
 *      generator_write_cryptsetup_unit_section). That import cannot deliver
 *      ours: PID 1 files a credential arriving from sd-stub under
 *      @encrypted and the import tries to decrypt plaintext, so the unit
 *      dies with status 243. The personal OS carries a drop-in that clears
 *      it and names the file with LoadCredential= instead (DESIGN Section 5,
 *      archusb's falcon-credential). The name below is what that drop-in
 *      loads.
 *   4. systemd-cryptsetup asks for the passphrase with
 *      `.credential = "cryptsetup.passphrase"` (src/cryptsetup/cryptsetup.c).
 *
 * So the file is named `cryptsetup.passphrase.cred` exactly. The name is
 * global rather than per-volume, which is what this loader needs: p4 is
 * unlocked by whatever unit the initramfs generates, and we do not get to
 * know that unit's name from in here.
 *
 * The bytes are the passphrase and nothing else: no trailing newline. The
 * consumer parses the credential with strv_parse_nulstr (ask-password-api.c),
 * so every byte up to the first NUL is the passphrase, a newline included. */
static const CHAR16 cred_file[] = L"cryptsetup.passphrase.cred";

/* Built once per launch from the chosen UKI's own name: the image, the
 * drop-in directory sd-stub derives from it, and the path LoadImage is given.
 * Static because they are ours for the whole life of the published
 * filesystem, which outlives the frame that filled them. */
static CHAR16 g_uki_ucs2[POS_NAME_MAX];
static CHAR16 g_extra_ucs2[POS_NAME_MAX + 16];
static CHAR16 g_load_ucs2[POS_NAME_MAX + 2];

static void build_names(const fs_dirent *ent)
{
    static const CHAR16 suffix[] = L".extra.d";
    UINTN n;

    name_to_ucs2(g_uki_ucs2, POS_NAME_MAX, ent->name, ent->name_len);

    /* The File Protocol path LoadImage resolves, absolute from our root. */
    g_load_ucs2[0] = L'\\';
    ucs2_copy(g_load_ucs2 + 1, POS_NAME_MAX + 1, g_uki_ucs2);

    /* sd-stub appends exactly this to the image's file path node. */
    n = ucs2_copy(g_extra_ucs2, POS_NAME_MAX, g_uki_ucs2);
    ucs2_copy(g_extra_ucs2 + n,
              sizeof(g_extra_ucs2) / sizeof(g_extra_ucs2[0]) - n, suffix);
}

/* Publishes image + drop-in directory + credential. Returns the publish
 * status; on failure nothing is published and the caller falls back.
 *
 * With no credential (a plaintext shelf: nothing was typed, so there is
 * nothing to hand on) only the image is published. The filesystem is still
 * worth publishing without it, because the device path it provides is what
 * LoadImage wants; what the boot gives up is the handoff, exactly as DESIGN
 * Section 3 says it does, and the initramfs prompts for p4 itself. */
#define LIST_W_(s) L##s
#define LIST_W(s) LIST_W_(s)
static const CHAR16 list_file[] = LIST_W(BLESSLIST_CRED);
static const CHAR16 ident_file[] = LIST_W(IDENT_CRED);
/* The boot partition's record as text, built per launch. */
static char  g_ident_text[IDENT_TEXT_MAX];
static UINTN g_ident_len;
static const void *g_list;
static UINTN       g_list_len;

void pos_set_bless_list(const void *data, UINTN len)
{
    g_list = (data != (const void *)0 && len != 0) ? data : (const void *)0;
    g_list_len = (g_list != (const void *)0) ? len : 0;
}

static EFI_STATUS publish_credfs(EFI_SYSTEM_TABLE *st, const fs_dirent *ent,
                                 void *buf, UINT64 size)
{
    credfs_entry tree[5];
    ident_rec rec;
    UINTN n = 2;

    build_names(ent);
    g_ident_len = 0;
    if (discover_install(&rec) == DISCOVER_ID_OK)
        g_ident_len = ident_encode_text(&rec, g_ident_text,
                                        sizeof g_ident_text);

    tree[0].name = g_uki_ucs2;
    tree[0].data = buf;
    tree[0].size = size;
    tree[0].parent = CREDFS_ROOT;
    tree[0].is_dir = 0;
    tree[0].secret = 0;

    tree[1].name = g_extra_ucs2;
    tree[1].data = (void *)0;
    tree[1].size = 0;
    tree[1].parent = CREDFS_ROOT;
    tree[1].is_dir = 1;
    tree[1].secret = 0;

    if (g_cred_len != 0) {
        tree[n].name = cred_file;
        tree[n].data = g_cred;
        tree[n].size = g_cred_len;
        tree[n].parent = (UINTN)1; /* the drop-in directory above */
        tree[n].is_dir = 0;
        /* The one entry credfs_teardown wipes. */
        tree[n].secret = 1;
        n++;
    }
    if (g_list_len != 0) {
        /* credfs serves the bytes and never writes them; the cast is the
         * entry type's, which is shared with the secret it does wipe. */
        tree[n].name = list_file;
        tree[n].data = (void *)(uintptr_t)g_list;
        tree[n].size = g_list_len;
        tree[n].parent = (UINTN)1;
        tree[n].is_dir = 0;
        tree[n].secret = 0;
        n++;
    }
    if (g_ident_len != 0) {
        tree[n].name = ident_file;
        tree[n].data = g_ident_text;
        tree[n].size = g_ident_len;
        tree[n].parent = (UINTN)1;
        tree[n].is_dir = 0;
        tree[n].secret = 0;
        n++;
    }
    /* Nothing to hand on: the image alone, as DESIGN Section 3 has it. */
    return credfs_publish(st, tree, (n > 2u) ? n : 1u);
}

/* LoadImage/StartImage of the UKI, by device path off the credential
 * filesystem when there is one, from the buffer when there is not. See pos.h
 * for why the device path is what this whole step exists to obtain. */
static EFI_STATUS launch_uki(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                             const fs_dirent *ent, void *buf, UINT64 size,
                             const load_ops *opsp, BOOLEAN inner_blocked,
                             UINT8 *retain, pos_result *result)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_DEVICE_PATH_PROTOCOL *load_path = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_HANDLE child = (EFI_HANDLE)0;
    CHAR16 *exit_data = (CHAR16 *)0;
    UINTN exit_size = 0;
    BOOLEAN published = FALSE;
    load_ops ops = *opsp;
    EFI_STATUS status;

    /* Set on the one path that leaves the UKI's pages published: see the
     * caller, which owns the allocation. The load ops were resolved by the
     * caller, before the read, so the warning that goes with them could be
     * printed while there was still a wait to spend it on. */
    *retain = 0;

    result->stage = L"publishing the credential filesystem";
    status = publish_credfs(st, ent, buf, size);
    if (!EFI_ERROR(status)) {
        published = TRUE;
        result->stage = L"naming the UKI on the credential filesystem";
        status = credfs_load_path(bs, g_load_ucs2, &load_path);
        if (EFI_ERROR(status)) {
            credfs_teardown(bs);
            published = FALSE;
        }
    }
    if (!published) {
        /* Loud, because the boot still works and the user is the only one who
         * can tell that it worked differently: the initramfs will ask for the
         * passphrase a second time. Not a security downgrade (the image is
         * verified identically either way), just a broken handoff. */
        out(L"warning: the credential filesystem could not be published; "
            L"booting from\r\n         memory instead. The initramfs will "
            L"ask for the passphrase again.\r\n\r\n");
    }

    /* Exactly one of DevicePath and SourceBuffer is set, which is what the
     * spec asks for. With a device path the started image gets a
     * LoadedImage->DeviceHandle pointing at our filesystem, which is what
     * sd-stub scans for its `.extra.d` companions. */
    /* Shim is here but not enforcing: load through the firmware, as
     * falcon-efi/src/iso/iso.c and bless.c do and for the reason argued
     * there (DESIGN Section 7.6). A UKI needs it as much: sd-stub loads its
     * kernel through the firmware's LoadImage with the UKI as the parent,
     * and an image shim's loader created is not a parent the firmware will
     * take, so with Secure Boot off every current kernel fails with
     * EFI_INVALID_PARAMETER. With Secure Boot on and
     * only shim's validation off the firmware still enforces db and refuses
     * a MOK-signed UKI; that is answered with shim's loader below, and the
     * inner-load warning the caller printed stands. */
    status = EFI_NOT_FOUND;   /* not loaded yet */
    if (ops.via_shim && ops.table_unhooked) {
        result->stage = L"LoadImage of the UKI (firmware)";
        status = bs->LoadImage(FALSE, parent, load_path,
                               published ? (void *)0 : buf,
                               published ? (UINTN)0 : (UINTN)size, &child);
        if (status == EFI_SUCCESS) {
            ops.LoadImage = bs->LoadImage;
            ops.StartImage = bs->StartImage;
            ops.UnloadImage = bs->UnloadImage;
            ops.via_shim = FALSE;
        } else {
            if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
                bs->UnloadImage(child);
            child = (EFI_HANDLE)0;
        }
    }
    if (status != EFI_SUCCESS) {
        result->stage = ops.via_shim ? L"LoadImage of the UKI (shim)"
                                     : L"LoadImage of the UKI (firmware)";
        status = ops.LoadImage(FALSE, parent, load_path,
                               published ? (void *)0 : buf,
                               published ? (UINTN)0 : (UINTN)size, &child);
    }
    if (load_path != (EFI_DEVICE_PATH_PROTOCOL *)0)
        bs->FreePool(load_path);
    if (EFI_ERROR(status)) {
        /* A refusal under Secure Boot still leaves a loaded image behind
         * (spec 7.4): the platform read and relocated it, then declined to
         * run it. The unloader has to be the one that matches the loader,
         * because shim tracks the images it loaded itself. */
        if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
            ops.UnloadImage(child);
        if (status == EFI_SECURITY_VIOLATION) {
            result->advice =
                L"the UKI is not signed with an enrolled key.\r\n\r\n"
                L"Sign it with the MOK key this machine has enrolled, turn "
                L"Secure Boot off\r\nin firmware setup, or run mokutil "
                L"--disable-validation from Linux.";
        }
        /* DESIGN Section 5.1: a failed load zeroes the credential and tears
         * the filesystem down before the menu comes back. */
        if (EFI_ERROR(credfs_teardown(bs)))
            *retain = 1;
        return status;
    }

    /* The handoff, said out loud: a UKI decompressing its kernel draws
     * nothing for a few seconds, and that silence is indistinguishable from
     * this loader having hung unless the boundary is on screen. Pairs with
     * the "the image exited" line main.c prints if it comes back. */
    if (g_handover != (void (*)(void))0)
        g_handover();
    out(L"handing over to the image; everything below is its own output."
        L"\r\n\r\n");

    result->stage = L"StartImage";
    loader_time_exec(st);
    status = ops.StartImage(child, &exit_size, &exit_data);
    bli_clear_exec(st);
    /* StartImage unloads an application that returns, so the child handle is
     * gone either way; only its exit data is ours to release. */
    if (exit_data != (CHAR16 *)0)
        bs->FreePool(exit_data);
    /* The firmware arms a watchdog around a started image; disarm it again so
     * a long stay in the menu afterwards cannot reset the machine. */
    bs->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);

    /* Reached only when the UKI returned instead of booting a kernel, which
     * for a real sd-stub image means it failed. The credential dies here too;
     * a teardown that could not uninstall leaves the module marked in use and
     * the secret unwiped on purpose (see credfs_teardown), so the failure is
     * worth naming rather than swallowing. */
    if (EFI_ERROR(credfs_teardown(bs))) {
        *retain = 1;
        result->stage = L"releasing the credential filesystem";
        result->status = EFI_ACCESS_DENIED;
        return EFI_ACCESS_DENIED;
    }

    result->stage = L"UKI exit";
    result->status = status;
    /* An sd-stub image that returns has failed, and in this state there is
     * one overwhelming reason for it that the stub's own message does not
     * name. */
    if (inner_blocked && EFI_ERROR(status))
        result->advice = load_ops_inner_load_advice();
    return EFI_SUCCESS;
}

/* ---- entry point ---- */

/* The one non-LUKS answer prompt_and_unlock can give. Positive, because every
 * LUKS status is zero or negative, so it cannot collide with one. */
#define POS_CANCELLED 1

/* Prompt, retry, and report. Split out so pos_boot's own exit path is one
 * wipe and one return regardless of which of the several ways in this can
 * end. Returns LUKS_OK with *which set, POS_CANCELLED if the user pressed
 * ESC, or the LUKS status to report. */
/* The wait after a wrong passphrase, with what was typed during it thrown
 * away: a key pressed while waiting is not an answer to the next prompt. */
static void unlock_wait(unsigned seconds)
{
    EFI_INPUT_KEY key;

    g_st->BootServices->Stall((UINTN)seconds * 1000000u);
    while (g_st->ConIn->ReadKeyStroke(g_st->ConIn, &key) == EFI_SUCCESS)
        ;
}

static int prompt_and_unlock(UINTN *which)
{
    for (;;) {
        INTN len;
        int rc;
        unsigned wait;

        if (g_note != (void (*)(void))0)
            g_note();
        out(L"passphrase for the encrypted partition: ");

        len = menu_prompt_secret(g_st, (const CHAR16 *)0, g_pass,
                                 POS_PASS_MAX);
        if (len < 0) {
            /* ESC. menu_prompt_secret already wiped its buffer; nothing was
             * attempted, so there is no key material to clear either. */
            return POS_CANCELLED;
        }
        g_pass_len = (UINTN)len;

        out(L"unlocking...\r\n");
        rc = unlock_any(which);
        if (rc == LUKS_OK)
            return LUKS_OK;
        if (rc != LUKS_ERR_NOKEY) {
            /* A device or format error is not something a retype fixes. */
            return rc;
        }
        crypto_wipe(g_pass, sizeof g_pass);
        g_pass_len = 0;
        g_failures++;
        /* The wait and the limit are not said (DESIGN Section 5.2): the wait
         * runs under "unlocking...", where it reads as a slow key
         * derivation, and the last wrong one powers off with the same
         * sentence as the others. */
        if (g_failures >= TUI_UNLOCK_TRIES) {
            out(L"wrong passphrase.\r\n");
            g_st->BootServices->Stall(3000000u);
            idle_power_off();
        }
        wait = tui_unlock_delay(g_failures);
        unlock_wait(wait);
        out(L"wrong passphrase.\r\n\r\n");
    }
}

void pos_set_station_hooks(void (*note)(void), void (*handover)(void))
{
    g_note = note;
    g_handover = handover;
}

const CHAR16 *pos_entry_label(void)
{
    /* The label is the promise the entry makes. An encrypted shelf will ask
     * for a passphrase and a plaintext one will not, and a user who sees
     * "encrypted" and is never asked has been told something false about
     * where their kernel lives. */
    if (g_luks_usable != 0)
        return L"" STROWS_LABEL_POS;
    if (g_plain_usable != 0)
        return L"Personal OS (plaintext shelf)";
    return L"Personal OS";
}

EFI_STATUS pos_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    pos_result *result)
{
    return pos_boot_pick(st, parent_image, POS_PICK_ASK, result);
}

/* Which shelf an attempt opens: POS_ANY is pos_boot's rule (an unlockable
 * encrypted shelf outranks a plaintext one), POS_ENCRYPTED the prompt, and
 * anything else that plaintext shelf's index in g_shelf. */
#define POS_ANY ((UINTN)-2)

static EFI_STATUS boot_shelf(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                             pos_pick want, UINTN shelf, const char *file,
                             pos_result *result);

EFI_STATUS pos_boot_pick(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                         pos_pick want, pos_result *result)
{
    return boot_shelf(st, parent_image, want, POS_ANY, (const char *)0,
                      result);
}

EFI_STATUS pos_boot_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                         UINTN shelf, const char *file, pos_result *result)
{
    return boot_shelf(st, parent_image, POS_PICK_ASK, shelf, file, result);
}

BOOLEAN pos_has_encrypted(void)
{
    return (BOOLEAN)(g_luks_usable != 0);
}

UINTN pos_plain_images(EFI_SYSTEM_TABLE *st, pos_image *out, UINTN cap)
{
    UINTN n = 0;
    UINTN i;

    g_st = st;
    if (out == (pos_image *)0)
        return 0;
    for (i = 0; i < g_shelf_n && n < cap; i++) {
        pos_shelf *p = &g_shelf[i];
        UINTN k;

        if (p->encrypted || !p->usable)
            continue;
        if (mount_shelf(discover_read, p->dev, p->fs) != FS_OK) {
            crypto_wipe(&g_fs, sizeof g_fs);
            continue;
        }
        if (shelf_collect(&g_images, &g_fs) == 1) {
            shelf_label(&g_images, &g_fs);
            for (k = 0; k < g_images.n && n < cap; k++) {
                const fs_dirent *e = &g_images.uki[k].ent;
                UINTN c;

                ucs2_copy(out[n].label, POS_LABEL_MAX, g_images.uki[k].label);
                /* A name that is not ASCII, or too long to carry, is not
                 * one a row can ask for again by name. */
                if (e->name_len == 0 || e->name_len >= POS_FILE_MAX)
                    continue;
                for (c = 0; c < e->name_len; c++) {
                    if (e->name[c] < 0x21u || e->name[c] > 0x7Eu)
                        break;
                    out[n].file[c] = (char)e->name[c];
                }
                if (c != e->name_len)
                    continue;
                out[n].file[c] = '\0';
                out[n].shelf = i;
                n++;
            }
        }
        crypto_wipe(&g_fs, sizeof g_fs);
        shelf_wipe(&g_images);
    }
    return n;
}

static EFI_STATUS boot_shelf(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                             pos_pick want, UINTN shelf, const char *file,
                             pos_result *result)
{
    EFI_BOOT_SERVICES *bs;
    menu_banner banner = { L"falcon-boot: which image?", (const CHAR16 *)0,
                           0 };
    void *buf = (void *)0;
    UINT64 size = 0;
    UINTN pages = 0;
    UINT8 retain = 0;
    UINTN which = 0;
    UINTN chosen;
    INTN pick;
    int rc;
    BOOLEAN inner_blocked = FALSE;
    load_ops ops;
    EFI_STATUS status = EFI_SUCCESS;
    /* Static for -Wstack-usage: a dirent carries a full name. */
    static fs_dirent launched;

    if (st == (void *)0 || result == (void *)0)
        return EFI_INVALID_PARAMETER;

    g_st = st;
    bs = st->BootServices;
    result->stage = L"starting";
    result->status = EFI_SUCCESS;
    result->advice = (const CHAR16 *)0;

    if (g_luks_usable == 0 && g_plain_usable == 0) {
        result->stage = L"looking for a UKI shelf";
        return EFI_NOT_FOUND;
    }
    if (shelf == POS_ENCRYPTED && g_luks_usable == 0) {
        result->stage = L"looking for an encrypted shelf";
        result->advice = L"no encrypted shelf of this install was found.";
        return EFI_NOT_FOUND;
    }
    if (shelf != POS_ENCRYPTED && shelf != POS_ANY &&
        (shelf >= g_shelf_n || g_shelf[shelf].encrypted ||
         !g_shelf[shelf].usable)) {
        result->stage = L"looking for a UKI shelf";
        return EFI_NOT_FOUND;
    }

    /* An unlockable encrypted shelf outranks a plaintext one. On a stick
     * built to DESIGN Section 3 there is only ever one; where both are in the
     * machine, the encrypted one is the one whose contents nobody else could
     * have read, so it is the one this entry means. A station row names its
     * shelf instead. */
    if (shelf == POS_ENCRYPTED || (shelf == POS_ANY && g_luks_usable != 0)) {
        result->stage = L"unlocking the encrypted partition";
        rc = prompt_and_unlock(&which);
        if (rc != LUKS_OK) {
            wipe_secrets();
            if (rc == POS_CANCELLED) {
                result->stage = L"passphrase prompt";
                return EFI_ABORTED;
            }
            if (rc == LUKS_ERR_NOKEY) {
                result->advice = L"the passphrase did not open any keyslot.";
                return EFI_ACCESS_DENIED;
            }
            if (rc == LUKS_ERR_IO) {
                result->advice = L"the partition could not be read.";
                return EFI_DEVICE_ERROR;
            }
            if (rc == LUKS_ERR_KDF_MEMORY) {
                result->advice = kdf_advice();
                return EFI_OUT_OF_RESOURCES;
            }
            result->advice =
                L"the volume is outside what this loader supports.";
            return EFI_UNSUPPORTED;
        }

        /* The passphrase has one more job than opening p3: it is also p4's
         * (DESIGN Section 3 gives them one passphrase precisely so this
         * handoff is "reuse the string the user already typed"). It travels
         * on as the credential, so it is copied into the buffer credfs will
         * serve and the buffer the user typed into dies right here, exactly
         * as early as it did before the handoff existed. From this point the
         * only live copy is g_cred, and credfs_teardown owns its wipe. */
        memcpy(g_cred, g_pass, sizeof g_cred);
        g_cred_len = g_pass_len;
        crypto_wipe(g_pass, sizeof g_pass);
        g_pass_len = 0;

        result->stage = L"opening the decrypted volume";
        if (luks_dev_open(&g_dev, &g_shelf[which].vol, &g_mk) != LUKS_OK) {
            wipe_secrets();
            return EFI_UNSUPPORTED;
        }
        g_dev_open = 1;
        /* luks_dev_open has taken the key into its cipher context, so the
         * copy here is dead from this point on. */
        crypto_wipe(&g_mk, sizeof g_mk);

        result->stage = L"mounting the filesystem inside it";
        rc = mount_shelf(luks_read, &g_dev, (const fs_ops *)0);
        if (rc != FS_OK) {
            result->advice = mount_advice(rc,
                L"the decrypted volume is not a filesystem this loader can "
                L"read.");
            status = EFI_UNSUPPORTED;
            goto out_close;
        }
    } else {
        /* DESIGN Section 3's plaintext shelf: no prompt, no credential, and
         * the initramfs asks for p4 itself. Everything below this point is
         * the same boot. */
        pos_shelf *p = (pos_shelf *)0;

        if (shelf != POS_ANY) {
            p = &g_shelf[shelf];
        } else {
            for (which = 0; which < g_shelf_n; which++) {
                if (!g_shelf[which].encrypted && g_shelf[which].usable) {
                    p = &g_shelf[which];
                    break;
                }
            }
        }
        if (p == (pos_shelf *)0) {
            result->stage = L"looking for a UKI shelf";
            return EFI_NOT_FOUND;
        }

        result->stage = L"mounting the plaintext UKI shelf";
        rc = mount_shelf(discover_read, p->dev, p->fs);
        if (rc != FS_OK) {
            result->advice = mount_advice(rc,
                L"the shelf is not a filesystem this loader can read.");
            status = EFI_UNSUPPORTED;
            goto out_close;
        }
    }

    result->stage = L"looking for a UKI";
    rc = shelf_collect(&g_images, &g_fs);
    if (rc != 1) {
        result->advice = L"no *.efi image was found in /uki or /EFI/Linux.";
        status = (rc < 0) ? EFI_DEVICE_ERROR : EFI_NOT_FOUND;
        goto out_close;
    }

    pick = choose_uki(&banner, want, file);
    if (pick == -2) {
        result->stage = L"looking for the UKI";
        result->advice = L"that UKI is not on the shelf.";
        status = EFI_NOT_FOUND;
        goto out_close;
    }
    if (pick < 0) {
        /* The appliance case never showed a chooser, so a negative answer
         * there is not a user cancelling: it is a shelf with no appliance on
         * it, which is a different thing to say and a different status. */
        if (want == POS_PICK_APPLIANCE) {
            result->stage = L"looking for the signing appliance";
            result->advice =
                L"no signing-appliance image is on this stick's shelf.\r\n"
                L"Bless the ISO on a Linux host with falcon-boot-sign "
                L"instead.";
            status = EFI_NOT_FOUND;
            goto out_close;
        }
        result->stage = L"choosing an image";
        status = EFI_ABORTED;
        goto out_close;
    }
    chosen = (UINTN)pick;

    menu_screen(st);

    /* The personal OS is a UKI, so it loads its kernel exactly the way a
     * blessed wrap does and is stopped by exactly the same platform state.
     * Judged before the read for the same reason bless.c judges it there: the
     * read is the slow part, and a warning that arrives after it is a
     * warning about a wait that has already been spent. The boot proceeds
     * regardless; this only decides what is said (DESIGN Section 7.3). */
    resolve_load_ops(bs, &ops);
    inner_blocked = load_ops_inner_load_blocked(st, &ops);
    if (inner_blocked)
        out(L"warning: this image is very unlikely to start on this machine;"
            L"\r\n         see the note after it exits.\r\n\r\n");

    /* Its OS lost power after an fsync: the journal holds fast commits,
     * which are not replayed (falcon-core/src/fs/jbd2.h), so what is read is
     * the shelf as it was a few seconds earlier. The boot goes on. */
    if ((g_jinfo.flags & FS_JNL_FC_SKIPPED) != 0)
        out(L"the shelf's last few seconds of changes were not read.\r\n");
    out(L"reading ");
    out_name(g_images.uki[chosen].ent.name, g_images.uki[chosen].ent.name_len);
    out(L" ");
    result->stage = L"reading the UKI";
    status = read_uki(bs, &g_images.uki[chosen].ent, &buf, &size, &pages);
    if (EFI_ERROR(status))
        goto out_close;
    out(L"\r\n");
    out_dec(size);
    /* The handoff line at the launch says "starting", at the moment it
     * becomes true. */
    out(L" bytes read.\r\n\r\n");

    /* p3 is done with: the UKI is in `buf`, and nothing after this reads
     * the shelf. The key schedule and the plaintext caches go now rather
     * than at out_close, because a boot that works never gets there. What
     * the launch still needs is the credential, which credfs wipes itself
     * (credfs.h, Lifetime), and the image's directory entry, whose name
     * credfs publishes it under. The entry is copied out first: wipe_shelf
     * zeroes the whole image list, and a launch handed the wiped entry
     * publishes an empty name, which credfs refuses, so the UKI boots from
     * memory without its credential and the initramfs asks again. */
    launched = g_images.uki[chosen].ent;
    wipe_shelf();

    status = launch_uki(st, parent_image, &launched, buf, size, &ops,
                        inner_blocked, &retain, result);
    crypto_wipe(&launched, sizeof launched);

    /* The image is loaded and running from the firmware's own copy by now, so
     * these pages are ours to release whether it succeeded or not. Doing it
     * here rather than leaking on the failure path is what lets a user retry
     * a refused entry without walking the machine out of memory.
     *
     * The exception is a credential filesystem that could not be taken down:
     * something still holds an interface into a filesystem whose one file IS
     * this buffer, so it is leaked on purpose. Leaking tens of megabytes once,
     * on a path that has already disabled further personal-OS boots, beats
     * handing a live reader pages the firmware may reissue to somebody else. */
    if (!retain)
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)buf, pages);

out_close:
    wipe_shelf();
    wipe_secrets();
    if (EFI_ERROR(status))
        result->status = status;
    return status;
}
