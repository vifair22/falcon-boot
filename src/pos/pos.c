/* The personal-OS path: passphrase -> p3 -> ext4 -> UKI -> shim. See pos.h
 * for the contract and, in particular, for why this path hands LoadImage a
 * buffer while the ISO path hands it a device path.
 */

#include <pos/pos.h>

#include <efi/blockio.h>
#include <efi/shimguid.h>

#include <crypto/crypto.h>
#include <ext4/ext4.h>
#include <luks/luks.h>
#include <lib/string.h>
#include <menu/menu.h>

/* p3 is one partition, but the scan is not told which one it is, so it takes
 * whatever LUKS2 volumes it finds. Four covers a stick built to DESIGN
 * Section 3 (p3 and p4) with room for a second stick plugged in beside it. */
#define POS_MAX_VOLS 4u

/* 256 bytes of passphrase plus the terminator. The limit is the prompt's, not
 * the format's; LUKS itself has no such cap. */
#define POS_PASS_MAX 257u

/* Wrong-passphrase retries before the attempt gives up and the menu comes
 * back. Re-prompting in place is what a user expects from an unlock prompt,
 * and there is nothing to rate-limit here: an attacker holding the stick
 * grinds the header offline, not through this prompt. */
#define POS_ATTEMPTS 3u

#define POS_MAX_UKIS 8u

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

/* Largest block size any firmware Block I/O instance we care about reports. */
#define POS_PROBE_BUF 4096u

static const uint8_t luks2_magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };

static EFI_SYSTEM_TABLE *g_st;

/* ---- console ---- */

static void out(const CHAR16 *s)
{
    /* Mutable parameter is the spec's wart, not ours; see src/efi/main.c. */
    g_st->ConOut->OutputString(g_st->ConOut, (CHAR16 *)(uintptr_t)s);
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

/* ext4 filenames are bytes and the console wants CHAR16. Everything the
 * loader will find on p3 is ASCII by construction (mkstick writes those
 * names), so anything else is shown as '?' rather than guessed at. */
static void name_to_ucs2(CHAR16 *dst, UINTN cap, const uint8_t *name,
                         UINTN len)
{
    UINTN i;

    for (i = 0; i < len && i + 1 < cap; i++)
        dst[i] = (name[i] >= 0x20 && name[i] < 0x7F) ? (CHAR16)name[i]
                                                     : L'?';
    dst[i] = 0;
}

static void out_name(const uint8_t *name, UINTN len)
{
    CHAR16 buf[64];

    name_to_ucs2(buf, sizeof(buf) / sizeof(buf[0]), name, len);
    out(buf);
}

/* ---- LUKS volumes found at scan time ---- */

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    luks_vol               vol;
    /* luks_open keeps views into this for the lifetime of vol, so it is
     * per-volume storage and not one shared scratch buffer. */
    char                   json[LUKS_JSON_CAP];
    uint8_t                usable;   /* has a pbkdf2 keyslot we can process */
} pos_luks;

static pos_luks g_luks[POS_MAX_VOLS];
static UINTN    g_luks_n;
static UINTN    g_luks_usable;

/* The secret, and the key it recovers. Both are file statics rather than
 * locals so that "everything that ever held key material" is two objects with
 * one wipe site each, rather than a set of stack frames nobody can audit. */
static char            g_pass[POS_PASS_MAX];
static UINTN           g_pass_len;
static luks_master_key g_mk;

static void wipe_secrets(void)
{
    crypto_wipe(g_pass, sizeof g_pass);
    crypto_wipe(&g_mk, sizeof g_mk);
    g_pass_len = 0;
}

/* Decrypted-volume and filesystem state. Static because each carries a
 * multi-kilobyte cache and -Wstack-usage polices frames at 32 KB. */
static luks_dev  g_dev;
static ext4_vol  g_fs;
static ext4_dir  g_dir_a;
static ext4_dir  g_dir_b;
static ext4_file g_file;

static ext4_dirent g_ukis[POS_MAX_UKIS];
static menu_entry  g_uki_entries[POS_MAX_UKIS];
static UINTN       g_uki_n;

/* ---- Block I/O plumbing ---- */

static int pos_bio_read(void *ctxv, uint64_t off, uint64_t len, void *buf)
{
    pos_luks *p = ctxv;
    UINT32 bs = p->bio->Media->BlockSize;

    /* LUKS metadata and the ext4 above it are read in whole 512-byte units,
     * so an unaligned request here is a driver bug rather than a device
     * condition, and is reported as such. */
    if (bs == 0 || off % bs != 0 || len % bs != 0)
        return -1;
    if (p->bio->ReadBlocks(p->bio, p->bio->Media->MediaId, off / bs,
                           (UINTN)len, buf) != EFI_SUCCESS)
        return -1;
    return 0;
}

/* Cheap first pass: is there a LUKS2 primary header here at all? Runs before
 * luks_open so that the common case (a firmware handle for something that is
 * not ours) costs one block read instead of a header parse and a SHA-256 over
 * the JSON area. */
static BOOLEAN has_luks_magic(EFI_BLOCK_IO_PROTOCOL *bio)
{
    static UINT8 blk[POS_PROBE_BUF];
    UINT32 bs = bio->Media->BlockSize;

    if (bs == 0 || bs > POS_PROBE_BUF)
        return FALSE;
    if (bio->ReadBlocks(bio, bio->Media->MediaId, 0, bs, blk) != EFI_SUCCESS)
        return FALSE;
    return (BOOLEAN)(memcmp(blk, luks2_magic, sizeof luks2_magic) == 0);
}

/* Does this volume have a keyslot this loader can actually process? DESIGN
 * Section 3 makes p3 pbkdf2 for exactly this reason; p4's argon2id slots are
 * recognised by the LUKS module and reported, never misread. */
static uint8_t has_pbkdf2_slot(const luks_vol *v)
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

UINTN pos_scan(EFI_SYSTEM_TABLE *st)
{
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = (EFI_HANDLE *)0;
    UINTN count = 0;
    UINTN i;

    g_st = st;
    g_luks_n = 0;
    g_luks_usable = 0;

    if (st->BootServices->LocateHandleBuffer(ByProtocol, &bio_guid,
                                             (void *)0, &count,
                                             &handles) != EFI_SUCCESS)
        return 0;

    for (i = 0; i < count && g_luks_n < POS_MAX_VOLS; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio;
        pos_luks *p = &g_luks[g_luks_n];

        if (st->BootServices->HandleProtocol(handles[i], &bio_guid,
                                             (void **)&bio) != EFI_SUCCESS)
            continue;
        if (!bio->Media->MediaPresent)
            continue;
        if (!has_luks_magic(bio))
            continue;

        p->bio = bio;
        p->usable = 0;
        if (luks_open(&p->vol, pos_bio_read, p, p->json,
                      LUKS_JSON_CAP) != LUKS_OK)
            continue;
        p->usable = has_pbkdf2_slot(&p->vol);
        if (p->usable)
            g_luks_usable++;
        g_luks_n++;
    }

    st->BootServices->FreePool(handles);
    return g_luks_usable;
}

/* ---- unlock ---- */

/* One passphrase against every unlockable volume.
 *
 * The same buffer is tried against each. DESIGN Section 3 gives p3 and p4 one
 * passphrase, and re-prompting per volume would make the user type the same
 * string twice to find out which partition the firmware happened to enumerate
 * first. Volumes with no pbkdf2 keyslot (p4) were already dropped by the
 * scan, so they are neither prompted for nor attempted.
 *
 * *which receives the index that opened. The distinction the caller needs is
 * "nobody accepted it", which is worth retrying, against "something broke",
 * which is not; so a NOKEY anywhere outranks a device error elsewhere,
 * because a mistyped passphrase is by far the likelier explanation and is the
 * one the user can act on. */
static int unlock_any(UINTN *which)
{
    int other_err = LUKS_OK;
    int saw_nokey = 0;
    UINTN i;

    *which = 0;
    for (i = 0; i < g_luks_n; i++) {
        uint32_t slot = 0;
        int rc;

        if (!g_luks[i].usable)
            continue;
        rc = luks_unlock(&g_luks[i].vol, g_pass, (size_t)g_pass_len, &g_mk,
                         &slot);
        if (rc == LUKS_OK) {
            *which = i;
            return LUKS_OK;
        }
        if (rc == LUKS_ERR_NOKEY)
            saw_nokey = 1;
        else if (other_err == LUKS_OK)
            other_err = rc;
    }

    if (saw_nokey)
        return LUKS_ERR_NOKEY;
    return (other_err != LUKS_OK) ? other_err : LUKS_ERR_NOKEY;
}

/* ---- finding the UKI on the ext4 inside p3 ---- */

/* Directory convention, fixed here and matched by mkstick (DESIGN Section 8):
 * the UKIs live in /uki. /EFI/Linux is accepted as a fallback because that is
 * where the Boot Loader Specification puts them, so a p3 populated by
 * `kernel-install` rather than by our tooling still boots. */
static int ascii_lower(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int name_is(const ext4_dirent *e, const char *want)
{
    UINTN n = (UINTN)e->name_len;
    UINTN i;

    for (i = 0; i < n; i++) {
        if (want[i] == '\0')
            return 0;
        if (ascii_lower(e->name[i]) != ascii_lower((uint8_t)want[i]))
            return 0;
    }
    return want[i] == '\0';
}

static int name_ends_efi(const ext4_dirent *e)
{
    static const char suffix[] = ".efi";
    UINTN n = sizeof(suffix) - 1u;
    UINTN len = (UINTN)e->name_len;
    UINTN i;

    if (len < n + 1u) /* ".efi" on its own is a name, not a UKI */
        return 0;
    for (i = 0; i < n; i++) {
        if (ascii_lower(e->name[len - n + i]) != (int)(uint8_t)suffix[i])
            return 0;
    }
    return 1;
}

/* Case-insensitive byte order, so the presented list does not depend on the
 * order the directory blocks happened to store the names in. */
static int name_cmp(const ext4_dirent *a, const ext4_dirent *b)
{
    UINTN la = (UINTN)a->name_len;
    UINTN lb = (UINTN)b->name_len;
    UINTN n = (la < lb) ? la : lb;
    UINTN i;

    for (i = 0; i < n; i++) {
        int ca = ascii_lower(a->name[i]);
        int cb = ascii_lower(b->name[i]);

        if (ca != cb)
            return (ca < cb) ? -1 : 1;
    }
    if (la == lb)
        return 0;
    return (la < lb) ? -1 : 1;
}

static int dir_find(ext4_dir *in, const char *name, ext4_dirent *found)
{
    ext4_dirent ent;
    int rc;

    while ((rc = ext4_dir_next(in, &ent)) == 1) {
        if (name_is(&ent, name)) {
            memcpy(found, &ent, sizeof ent);
            return 1;
        }
    }
    return rc; /* 0 = not found, negative = the driver's error */
}

/* Opens the directory the UKIs live in, leaving it in *d. Returns 1 when one
 * was found, 0 when neither convention is present, negative on an ext4
 * error. */
static int open_uki_dir(ext4_dir *d)
{
    ext4_dirent ent;
    int rc;

    if (ext4_dir_open_root(&g_fs, &g_dir_a) != EXT4_OK)
        return -1;
    rc = dir_find(&g_dir_a, "uki", &ent);
    if (rc == 1 && ent.is_dir)
        return (ext4_dir_open(&g_fs, d, &ent) == EXT4_OK) ? 1 : -1;
    if (rc < 0)
        return rc;

    /* Fallback: /EFI/Linux, the Boot Loader Specification's location. */
    if (ext4_dir_open_root(&g_fs, &g_dir_a) != EXT4_OK)
        return -1;
    rc = dir_find(&g_dir_a, "EFI", &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    if (ext4_dir_open(&g_fs, &g_dir_b, &ent) != EXT4_OK)
        return -1;
    rc = dir_find(&g_dir_b, "Linux", &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    return (ext4_dir_open(&g_fs, d, &ent) == EXT4_OK) ? 1 : -1;
}

/* Collects the regular *.efi files in the UKI directory, sorted by name.
 * Anything else in there (the decoy the fixture drops in, a README, a
 * subdirectory) is skipped rather than offered: a menu entry that cannot be a
 * kernel is worse than no entry.
 *
 * g_dir_b is both open_uki_dir's scratch for the /EFI level and the directory
 * it finally leaves open; that is safe only because every dirent it acts on is
 * copied out before the next open overwrites the iterator. */
static int collect_ukis(void)
{
    ext4_dir *d = &g_dir_b;
    ext4_dirent ent;
    int rc;

    g_uki_n = 0;
    rc = open_uki_dir(d);
    if (rc != 1)
        return rc;

    while ((rc = ext4_dir_next(d, &ent)) == 1) {
        UINTN j;

        if (!ent.is_regular || !name_ends_efi(&ent))
            continue;
        if (g_uki_n >= POS_MAX_UKIS)
            break;

        /* Insertion sort as they arrive; the list is at most a handful. */
        j = g_uki_n++;
        while (j > 0 && name_cmp(&g_ukis[j - 1], &ent) > 0) {
            memcpy(&g_ukis[j], &g_ukis[j - 1], sizeof ent);
            j--;
        }
        memcpy(&g_ukis[j], &ent, sizeof ent);
    }
    if (rc < 0)
        return rc;
    return (g_uki_n > 0) ? 1 : 0;
}

/* Which UKI to boot. One is booted without asking; several are offered,
 * because on a stick built to DESIGN Section 7.5 the second one is the
 * signing appliance and picking for the user would be picking wrong half the
 * time. Returns the index, or a negative value if the user backed out. */
static INTN choose_uki(const menu_banner *banner)
{
    UINTN i;

    if (g_uki_n == 1)
        return 0;

    for (i = 0; i < g_uki_n; i++) {
        static const CHAR16 prefix[] = L"Personal OS: ";
        UINTN pl = sizeof(prefix) / sizeof(prefix[0]) - 1;

        memcpy(g_uki_entries[i].label, prefix, pl * sizeof(CHAR16));
        name_to_ucs2(g_uki_entries[i].label + pl, MENU_PATH_MAX - pl,
                     g_ukis[i].name, g_ukis[i].name_len);
    }
    return menu_run(g_st, banner, g_uki_entries, g_uki_n);
}

/* ---- the shim-verified load path (DESIGN Section 7.6) ---- */

/* Deliberately a second copy of src/iso/iso.c's resolve_load_ops rather than a
 * shared helper. Sharing it would mean either exporting it from iso.h (making
 * the ISO module's public surface carry a facility that has nothing to do with
 * ISOs) or adding a module for twenty lines. Both cost more than the
 * duplication does, and the two copies cannot drift in a way that matters:
 * each is a straight-line read of one protocol, and the invariant they encode
 * (prefer shim's loader, fall back to the firmware's, never install our own)
 * is stated in DESIGN Section 7.6 and holds by review, not by a symbol. If a
 * third caller ever appears, promote it then. */
typedef struct {
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN, EFI_HANDLE,
                                   EFI_DEVICE_PATH_PROTOCOL *, void *, UINTN,
                                   EFI_HANDLE *);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE);
    BOOLEAN via_shim;
} load_ops;

static void resolve_load_ops(EFI_BOOT_SERVICES *bs, load_ops *ops)
{
    EFI_GUID sil_guid = SHIM_IMAGE_LOADER_GUID;
    SHIM_IMAGE_LOADER *sil = (SHIM_IMAGE_LOADER *)0;

    ops->LoadImage = bs->LoadImage;
    ops->StartImage = bs->StartImage;
    ops->UnloadImage = bs->UnloadImage;
    ops->via_shim = FALSE;

    if (bs->LocateProtocol(&sil_guid, (void *)0, (void **)&sil) != EFI_SUCCESS)
        return;
    /* A protocol that is present but hollow is a shim we do not understand;
     * the firmware's loader is the safe answer, not a null call. */
    if (sil == (SHIM_IMAGE_LOADER *)0 || sil->LoadImage == (void *)0 ||
        sil->StartImage == (void *)0 || sil->UnloadImage == (void *)0)
        return;

    ops->LoadImage = sil->LoadImage;
    ops->StartImage = sil->StartImage;
    ops->UnloadImage = sil->UnloadImage;
    ops->via_shim = TRUE;
}

/* Reads the whole UKI into freshly allocated pages.
 *
 * Whole, not streamed: LoadImage takes a buffer and a length, so the image
 * has to be contiguous in memory before the call. Pages rather than pool
 * because the buffer is tens of megabytes and page-aligned is what every PE
 * loader wants to start from. */
static EFI_STATUS read_uki(EFI_BOOT_SERVICES *bs, const ext4_dirent *ent,
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

    if (ext4_file_open(&g_fs, &g_file, ent) != EXT4_OK)
        return EFI_DEVICE_ERROR;
    size = ext4_file_size(&g_file);
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
        if (ext4_file_read(&g_file, done, n, buf + done) != EXT4_OK) {
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

/* LoadImage/StartImage of the bytes we just read. See pos.h for why a buffer
 * is the right and only shape here, and why that is not the ISO path's rule
 * being bent. */
static EFI_STATUS launch_uki(EFI_BOOT_SERVICES *bs, EFI_HANDLE parent,
                             void *buf, UINT64 size, pos_result *result)
{
    EFI_HANDLE child = (EFI_HANDLE)0;
    CHAR16 *exit_data = (CHAR16 *)0;
    UINTN exit_size = 0;
    load_ops ops;
    EFI_STATUS status;

    resolve_load_ops(bs, &ops);

    /* DevicePath NULL, SourceBuffer set. A consequence worth knowing before
     * step 6 lands: the started image's LoadedImage->DeviceHandle is NULL,
     * because there is no device behind these bytes. The stand-in payload
     * does not care. A real sd-stub UKI does: its companion-credential pickup
     * (DESIGN Section 5.1) scans the directory next to the image, which needs
     * a device to scan. That is exactly what the synthetic filesystem of step
     * 6 exists to provide, and it replaces this call's NULL device path. */
    result->stage = ops.via_shim ? L"LoadImage of the UKI (shim)"
                                 : L"LoadImage of the UKI (firmware)";
    status = ops.LoadImage(FALSE, parent, (EFI_DEVICE_PATH_PROTOCOL *)0, buf,
                           (UINTN)size, &child);
    if (EFI_ERROR(status)) {
        /* A refusal under Secure Boot still leaves a loaded image behind
         * (spec 7.4): the platform read and relocated it, then declined to
         * run it. The unloader has to be the one that matches the loader,
         * because shim tracks the images it loaded itself. */
        if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
            ops.UnloadImage(child);
        if (status == EFI_SECURITY_VIOLATION) {
            result->advice =
                L"the UKI is not signed with an enrolled key. Sign it with "
                L"the MOK key\r\nthis machine has enrolled, or opt out of "
                L"enforcement in firmware setup\r\n(Secure Boot off) or with "
                L"mokutil --disable-validation.";
        }
        return status;
    }

    result->stage = L"StartImage";
    status = ops.StartImage(child, &exit_size, &exit_data);
    /* StartImage unloads an application that returns, so the child handle is
     * gone either way; only its exit data is ours to release. */
    if (exit_data != (CHAR16 *)0)
        bs->FreePool(exit_data);
    /* The firmware arms a watchdog around a started image; disarm it again so
     * a long stay in the menu afterwards cannot reset the machine. */
    bs->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);

    result->stage = L"UKI exit";
    result->status = status;
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
static int prompt_and_unlock(UINTN *which)
{
    UINTN attempt;

    for (attempt = 0; attempt < POS_ATTEMPTS; attempt++) {
        INTN len;
        int rc;

        out(L"passphrase for the encrypted partition");
        if (attempt > 0) {
            out(L" (attempt ");
            out_dec(attempt + 1);
            out(L" of ");
            out_dec(POS_ATTEMPTS);
            out(L")");
        }
        out(L": ");

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
        out(L"wrong passphrase.\r\n\r\n");
    }
    return LUKS_ERR_NOKEY;
}

EFI_STATUS pos_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                    pos_result *result)
{
    EFI_BOOT_SERVICES *bs;
    menu_banner banner = { L"airies-boot: which image?", (const CHAR16 *)0,
                           0 };
    void *buf = (void *)0;
    UINT64 size = 0;
    UINTN pages = 0;
    UINTN which = 0;
    UINTN chosen;
    INTN pick;
    int rc;
    EFI_STATUS status = EFI_SUCCESS;

    if (st == (void *)0 || result == (void *)0)
        return EFI_INVALID_PARAMETER;

    g_st = st;
    bs = st->BootServices;
    result->stage = L"starting";
    result->status = EFI_SUCCESS;
    result->advice = (const CHAR16 *)0;

    if (g_luks_usable == 0) {
        result->stage = L"looking for an unlockable LUKS2 volume";
        return EFI_NOT_FOUND;
    }

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
        result->advice = L"the volume is outside what this loader supports.";
        return EFI_UNSUPPORTED;
    }

    /* The passphrase has done its one job for this step. Step 6 is where it
     * stops dying here and starts travelling on to the initramfs through the
     * synthetic credential filesystem (DESIGN Section 5.1); until then,
     * holding it a moment longer buys nothing. */
    crypto_wipe(g_pass, sizeof g_pass);
    g_pass_len = 0;

    result->stage = L"opening the decrypted volume";
    if (luks_dev_open(&g_dev, &g_luks[which].vol, &g_mk) != LUKS_OK) {
        wipe_secrets();
        return EFI_UNSUPPORTED;
    }
    /* luks_dev_open has taken the key into its cipher context, so the copy
     * here is dead from this point on. */
    crypto_wipe(&g_mk, sizeof g_mk);

    result->stage = L"mounting the ext4 filesystem inside it";
    if (ext4_mount(&g_fs, luks_read, &g_dev) != EXT4_OK) {
        result->advice = L"the decrypted volume is not a filesystem this "
                         L"loader can read.";
        status = EFI_UNSUPPORTED;
        goto out_close;
    }

    result->stage = L"looking for a UKI";
    rc = collect_ukis();
    if (rc != 1) {
        result->advice = L"no *.efi image was found in /uki or /EFI/Linux.";
        status = (rc < 0) ? EFI_DEVICE_ERROR : EFI_NOT_FOUND;
        goto out_close;
    }

    pick = choose_uki(&banner);
    if (pick < 0) {
        result->stage = L"choosing an image";
        status = EFI_ABORTED;
        goto out_close;
    }
    chosen = (UINTN)pick;

    st->ConOut->ClearScreen(st->ConOut);
    out(L"reading ");
    out_name(g_ukis[chosen].name, g_ukis[chosen].name_len);
    out(L" ");
    result->stage = L"reading the UKI";
    status = read_uki(bs, &g_ukis[chosen], &buf, &size, &pages);
    if (EFI_ERROR(status))
        goto out_close;
    out(L"\r\n");
    out_dec(size);
    out(L" bytes read; starting it.\r\n\r\n");

    status = launch_uki(bs, parent_image, buf, size, result);

    /* The image is loaded and running from the firmware's own copy by now, so
     * these pages are ours to release whether it succeeded or not. Doing it
     * here rather than leaking on the failure path is what lets a user retry
     * a refused entry without walking the machine out of memory. */
    bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)buf, pages);

out_close:
    /* The filesystem caches hold plaintext read out of an encrypted volume.
     * It is not key material, so nothing turns on it, but leaving fragments
     * of p3 sitting in firmware pool memory for whatever boots next is not a
     * thing to do by omission. crypto_wipe rather than memset because these
     * are exactly the dead-buffer stores an optimiser is entitled to drop. */
    crypto_wipe(&g_fs, sizeof g_fs);
    crypto_wipe(&g_dir_a, sizeof g_dir_a);
    crypto_wipe(&g_dir_b, sizeof g_dir_b);
    crypto_wipe(&g_file, sizeof g_file);
    luks_dev_close(&g_dev);
    wipe_secrets();
    if (EFI_ERROR(status))
        result->status = status;
    return status;
}
