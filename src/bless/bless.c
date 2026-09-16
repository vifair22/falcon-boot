/* Blessed boot: manifest -> signed UKI wrap -> synthetic filesystem -> shim.
 * See bless.h for the contract and blesscore.h for why nothing in the
 * manifest is trusted.
 */

#include <bless/bless.h>

#include <efi/loadops.h>

#include <credfs/credfs.h>
#include <lib/string.h>
#include <menu/menu.h>

/* Read granularity for the UKI, and how often the read says it is alive. A
 * 1 MiB request costs one directory-to-extent descent and a run of block
 * reads; the dot is per 8 MiB so a 200 MB wrap draws a short line rather than
 * a wall. Both match src/pos, which reads the same kind of object. */
#define BLESS_READ_CHUNK ((UINT64)1u << 20)
#define BLESS_DOT_EVERY  ((UINT64)8u << 20)

/* Room for the longest name a filesystem here can hold, as CHAR16. The
 * synthetic filesystem reuses the UKI's own name so an operator reading a
 * failure line sees the file the manifest named rather than a placeholder. */
#define BLESS_NAME_MAX 256u

static EFI_SYSTEM_TABLE *g_st;

/* The records, and the volumes they were read from. Static because the cache
 * is ~16 KB and the loader's frames are policed at 32 KB, and because it
 * outlives every call here: a menu entry is booted long after the scan that
 * labelled it. */
static bless_cache g_cache;

/* Directory iterators and the open file. Static for the same reason src/pos
 * keeps its own: each carries a multi-kilobyte driver cache. */
static fs_dir  g_dir_a;
static fs_dir  g_dir_b;
static fs_file g_file;

/* ---- console ---- */

/* Inside the content block, like every other screen; see src/menu/menu.h. */
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

/* The manifest's names are printable ASCII by the format's own rule
 * (manifest_parse drops any record that is not), so widening a byte at a time
 * is the whole conversion and there is no lossy case to handle. */
static void ascii_to_ucs2(CHAR16 *dst, UINTN cap, const char *src)
{
    UINTN i = 0;

    if (cap == 0)
        return;
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

/* The name of the image being launched, as the console and the synthetic
 * filesystem both want it.
 *
 * One buffer, filled by one of the two setters below and used by both
 * consumers, because the name printed while the image is read must be the
 * name the filesystem then serves: two conversions of the same record could
 * not disagree today, but the whole point of naming the file on screen is
 * that it is the file that was used. */
static CHAR16 g_uki_ucs2[BLESS_NAME_MAX];

/* From a manifest record's uki=, which is ASCII by the format's own rule. */
static const CHAR16 *set_name_ascii(const char *uki_name)
{
    ascii_to_ucs2(g_uki_ucs2, BLESS_NAME_MAX, uki_name);
    return g_uki_ucs2;
}

/* From a directory entry, which is already CHAR16: a bare EFI payload is
 * named by the file on p2 and never by a record (DESIGN Section 7.2). */
static const CHAR16 *set_name_ucs2(const CHAR16 *name)
{
    ucs2_copy(g_uki_ucs2, BLESS_NAME_MAX, name);
    return g_uki_ucs2;
}

/* ---- finding things under /falcon/blessed ---- */

static int dir_find(fs_dir *in, const char *name, fs_dirent *found)
{
    fs_dirent ent;
    int rc;

    while ((rc = fs_dir_next(in, &ent)) == 1) {
        if (fs_name_eq_ascii(ent.name, name)) {
            memcpy(found, &ent, sizeof ent);
            return 1;
        }
    }
    return rc; /* 0 = not found, negative = the driver's error */
}

/* Opens /falcon/blessed on the volume, leaving it in *d. Returns 1 when it is
 * there, 0 when it is not, negative on a filesystem error.
 *
 * g_dir_a is the scratch for the /falcon level; the caller's *d is where the
 * walk ends up. Safe only because the dirent is copied out of the iterator
 * before the next open overwrites it, which dir_find guarantees. */
static int open_blessed_dir(fs_vol *vol, fs_dir *d)
{
    fs_dirent ent;
    int rc;

    if (fs_dir_open_root(vol, &g_dir_a) != FS_OK)
        return -1;
    rc = dir_find(&g_dir_a, BLESS_DIR_L1, &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    if (fs_dir_open(vol, &g_dir_a, &ent) != FS_OK)
        return -1;
    rc = dir_find(&g_dir_a, BLESS_DIR_L2, &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    return (fs_dir_open(vol, d, &ent) == FS_OK) ? 1 : -1;
}

/* Opens /falcon/prepared/<sha256> on the volume, leaving it in *d. Same
 * contract as open_blessed_dir, one level deeper: 1 when it is there, 0 when
 * it is not, negative on a filesystem error.
 *
 * g_dir_a and the caller's *d take turns holding the level being read, so no
 * open ever writes through the iterator it is reading from. The last component
 * is the record's sha256=, never a user-chosen name (DESIGN Section 6.3.1). */
static int open_prepared_dir(fs_vol *vol, const char *sha, fs_dir *d)
{
    fs_dirent ent;
    int rc;

    if (fs_dir_open_root(vol, &g_dir_a) != FS_OK)
        return -1;
    rc = dir_find(&g_dir_a, BLESS_DIR_L1, &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    if (fs_dir_open(vol, d, &ent) != FS_OK)
        return -1;
    rc = dir_find(d, BLESS_DIR_PREPARED, &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    if (fs_dir_open(vol, &g_dir_a, &ent) != FS_OK)
        return -1;
    rc = dir_find(&g_dir_a, sha, &ent);
    if (rc != 1 || !ent.is_dir)
        return (rc < 0) ? rc : 0;
    return (fs_dir_open(vol, d, &ent) == FS_OK) ? 1 : -1;
}

/* ---- the manifest ---- */

void bless_reset(void)
{
    bless_cache_init(&g_cache);
}

UINTN bless_load(EFI_SYSTEM_TABLE *st, fs_vol *vol)
{
    EFI_BOOT_SERVICES *bs;
    fs_dirent ent;
    void *buf = (void *)0;
    UINT64 size;
    UINTN added;

    if (st == (EFI_SYSTEM_TABLE *)0 || vol == (fs_vol *)0)
        return 0;
    g_st = st;
    bs = st->BootServices;

    /* Every early return below is "nothing is blessed on this volume", which
     * Section 8.1 makes indistinguishable from "there is no manifest". A
     * volume that is not a stick built by our tooling takes the first one. */
    if (open_blessed_dir(vol, &g_dir_b) != 1)
        return 0;
    if (dir_find(&g_dir_b, BLESS_MANIFEST_NAME, &ent) != 1 || ent.is_dir)
        return 0;
    if (fs_file_open(vol, &g_file, &ent) != FS_OK)
        return 0;

    size = fs_file_size(&g_file);
    if (size == 0)
        return 0;
    /* Truncated rather than refused: the records at the front of an
     * over-long file are still records, and the alternative is that padding a
     * manifest disables every blessing on the stick. */
    if (size > BLESS_MANIFEST_MAX)
        size = BLESS_MANIFEST_MAX;

    /* Pool rather than static storage: 64 KiB that is live for the length of
     * one parse should not sit in the loader's BSS for the whole boot. */
    if (bs->AllocatePool(EfiLoaderData, (UINTN)size, &buf) != EFI_SUCCESS)
        return 0;
    if (fs_file_read(&g_file, 0, size, buf) != FS_OK) {
        bs->FreePool(buf);
        return 0;
    }

    added = bless_cache_add(&g_cache, buf, (size_t)size, vol);
    bs->FreePool(buf);
    return added;
}

bless_state bless_lookup(const fs_vol *vol, const CHAR16 *path,
                         UINT64 cur_size, bless_hit *out)
{
    return bless_cache_lookup(&g_cache, vol, path, cur_size, out);
}

UINTN bless_dropped(void)
{
    return g_cache.dropped;
}

/* ---- the shim-verified load path (DESIGN Section 7.6) ---- */


/* ---- reading the wrap ---- */

/* Reads the whole image into freshly allocated pages.
 *
 * Whole, not streamed: LoadImage takes a buffer and a length, so the image has
 * to be contiguous in memory before the call. Pages rather than pool because
 * a blessed wrap carries an embedded initramfs (Section 8.1) and is hundreds
 * of megabytes, and page-aligned is what every PE loader wants to start from. */
static EFI_STATUS read_image(EFI_BOOT_SERVICES *bs, fs_vol *vol,
                             const fs_dirent *ent, void **buf_out,
                             UINT64 *size_out, UINTN *pages_out)
{
    EFI_PHYSICAL_ADDRESS addr = 0;
    UINT64 size;
    UINT64 done = 0;
    UINT64 next_dot = BLESS_DOT_EVERY;
    UINTN pages;
    UINT8 *buf;
    EFI_STATUS status;

    *buf_out = (void *)0;
    *size_out = 0;
    *pages_out = 0;

    if (fs_file_open(vol, &g_file, ent) != FS_OK)
        return EFI_DEVICE_ERROR;
    size = fs_file_size(&g_file);
    if (size == 0 || size > BLESS_UKI_MAX)
        return EFI_UNSUPPORTED;

    pages = (UINTN)((size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE);
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(status))
        return status;
    buf = (UINT8 *)(uintptr_t)addr;

    while (done < size) {
        UINT64 n = size - done;

        if (n > BLESS_READ_CHUNK)
            n = BLESS_READ_CHUNK;
        if (fs_file_read(&g_file, done, n, buf + done) != FS_OK) {
            bs->FreePages(addr, pages);
            return EFI_DEVICE_ERROR;
        }
        done += n;
        if (done >= next_dot) {
            out(L".");
            next_dot += BLESS_DOT_EVERY;
        }
    }

    *buf_out = buf;
    *size_out = size;
    *pages_out = pages;
    return EFI_SUCCESS;
}

/* ---- the launch (DESIGN Sections 5.1 and 7.6) ---- */

/* The path LoadImage is given, built from the UKI's own name. Static for the
 * same reason g_uki_ucs2 is: it is ours for the whole life of the published
 * filesystem, which outlives the frame that filled it. */
static CHAR16 g_load_ucs2[BLESS_NAME_MAX + 2];

/* Publishes the image, and only the image, under the name already in
 * g_uki_ucs2.
 *
 * No credential and no `<uki>.extra.d` directory: a blessed wrap carries its
 * own initrd and cmdline inside the signature (Section 8.1, amended), so
 * there is nothing to hand to it and nothing it needs to be told. The
 * synthetic filesystem is still what the launch is built on, for the reason
 * src/credfs exists at all: a device-path load is the only kind that gives
 * the started image a LoadedImage->DeviceHandle, and exFAT has no firmware
 * device path of its own to offer. That reason is what makes this the
 * mechanism a bare EFI payload uses too (DESIGN Section 7.2): it lives on the
 * same unreadable-to-firmware exFAT and needs the same device path. */
static EFI_STATUS publish_image(EFI_SYSTEM_TABLE *st, void *buf, UINT64 size)
{
    credfs_entry tree[1];

    /* The File Protocol path LoadImage resolves, absolute from our root. */
    g_load_ucs2[0] = L'\\';
    ucs2_copy(g_load_ucs2 + 1, BLESS_NAME_MAX + 1, g_uki_ucs2);

    tree[0].name = g_uki_ucs2;
    tree[0].data = buf;
    tree[0].size = size;
    tree[0].parent = CREDFS_ROOT;
    tree[0].is_dir = 0;
    tree[0].secret = 0;

    return credfs_publish(st, tree, 1u);
}

/* LoadImage/StartImage of the image in g_uki_ucs2, by device path off the
 * synthetic filesystem when it could be published, from the buffer when it
 * could not.
 *
 * refused is the one line to print when the platform answers
 * EFI_SECURITY_VIOLATION. It is a parameter because that is the only thing
 * the two callers differ in: for a blessed wrap the image IS ours and the
 * actionable half is the key, and for a bare payload the actionable half is
 * that the file itself has not been signed yet.
 *
 * *retain is set on the one path that leaves the image's pages published: see
 * the caller, which owns the allocation. */
static EFI_STATUS launch_image(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                               void *buf, UINT64 size,
                               const load_ops *opsp, BOOLEAN inner_blocked,
                               const CHAR16 *refused, UINT8 *retain,
                               bless_result *result)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_DEVICE_PATH_PROTOCOL *load_path = (EFI_DEVICE_PATH_PROTOCOL *)0;
    EFI_HANDLE child = (EFI_HANDLE)0;
    CHAR16 *exit_data = (CHAR16 *)0;
    UINTN exit_size = 0;
    BOOLEAN published = FALSE;
    load_ops ops = *opsp;
    EFI_STATUS status;

    /* Resolved by the caller, before the read, so the warning that goes with
     * it can be on screen for the whole of a read that takes a while. */
    *retain = 0;

    result->stage = L"publishing the image";
    status = publish_image(st, buf, size);
    if (!EFI_ERROR(status)) {
        published = TRUE;
        result->stage = L"naming the image on the synthetic filesystem";
        status = credfs_load_path(bs, g_load_ucs2, &load_path);
        if (EFI_ERROR(status)) {
            credfs_teardown(bs);
            published = FALSE;
        }
    }
    /* A buffer load is a complete fallback here, unlike on the personal-OS
     * path: nothing is being handed to this image through the filesystem, so
     * all a failed publish costs is the DeviceHandle, and sd-stub needs that
     * only to find companions this wrap does not have. Same verification
     * either way, which is the only property that must not move. */

    /* Exactly one of DevicePath and SourceBuffer is set, which is what the
     * spec asks for. */
    result->stage = ops.via_shim ? L"LoadImage of the image (shim)"
                                 : L"LoadImage of the image (firmware)";
    status = ops.LoadImage(FALSE, parent, load_path,
                           published ? (void *)0 : buf,
                           published ? (UINTN)0 : (UINTN)size, &child);
    if (load_path != (EFI_DEVICE_PATH_PROTOCOL *)0)
        bs->FreePool(load_path);
    if (EFI_ERROR(status)) {
        /* A refusal under Secure Boot still leaves a loaded image behind
         * (spec 7.4): the platform read and relocated it, then declined to
         * run it. The unloader has to be the one that matches the loader,
         * because shim tracks the images it loaded itself. */
        if (status == EFI_SECURITY_VIOLATION && child != (EFI_HANDLE)0)
            ops.UnloadImage(child);
        if (status == EFI_SECURITY_VIOLATION)
            result->advice = refused;
        if (published && EFI_ERROR(credfs_teardown(bs)))
            *retain = 1;
        return status;
    }

    /* The handoff, said out loud: everything after this call is somebody
     * else's output, and a wrap that spends seconds decompressing a kernel
     * before it draws anything is indistinguishable from a hang unless the
     * boundary is on screen. Pairs with the "exited" line main.c prints if it
     * comes back. */
    out(L"handing over to the image; everything below is its own output."
        L"\r\n\r\n");

    result->stage = L"StartImage";
    status = ops.StartImage(child, &exit_size, &exit_data);
    /* StartImage unloads an application that returns, so the child handle is
     * gone either way; only its exit data is ours to release. */
    if (exit_data != (CHAR16 *)0)
        bs->FreePool(exit_data);
    /* The firmware arms a watchdog around a started image; disarm it again so
     * a long stay in the menu afterwards cannot reset the machine. */
    bs->SetWatchdogTimer(0, 0, 0, (CHAR16 *)0);

    /* Reached only when the UKI returned instead of booting a kernel, which
     * for a real sd-stub image means it failed. A teardown that could not
     * uninstall leaves the module marked in use, so the failure is worth
     * naming rather than swallowing. */
    if (published && EFI_ERROR(credfs_teardown(bs))) {
        *retain = 1;
        result->stage = L"releasing the synthetic filesystem";
        result->status = EFI_ACCESS_DENIED;
        return EFI_ACCESS_DENIED;
    }

    result->stage = L"image exit";
    result->status = status;
    /* A UKI that returns at all has failed (it should have left for a
     * kernel). In this state there is one overwhelming reason for it, the
     * loader knows it, and the stub's own message names shim without
     * mentioning the setting that actually did it. A bare payload that
     * returns has merely finished, and inner_blocked cannot be set for one:
     * the predicate is about an image loading a PE of its own, which is what
     * sd-stub does and what a self-contained payload by definition does not. */
    if (inner_blocked && EFI_ERROR(status))
        result->advice = load_ops_inner_load_advice();
    return EFI_SUCCESS;
}

EFI_STATUS bless_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                      const bless_hit *hit, bless_result *result)
{
    EFI_BOOT_SERVICES *bs;
    fs_vol *vol;
    fs_dirent ent;
    void *buf = (void *)0;
    UINT64 size = 0;
    UINTN pages = 0;
    UINT8 retain = 0;
    BOOLEAN inner_blocked = FALSE;
    load_ops ops;
    EFI_STATUS status;

    if (st == (EFI_SYSTEM_TABLE *)0 || result == (bless_result *)0)
        return EFI_INVALID_PARAMETER;

    g_st = st;
    bs = st->BootServices;
    result->stage = L"starting";
    result->status = EFI_SUCCESS;
    result->advice = (const CHAR16 *)0;

    if (hit == (const bless_hit *)0 ||
        hit->rec == (const manifest_record *)0 ||
        hit->src == (const void *)0)
        return EFI_INVALID_PARAMETER;
    /* The tag came out of this module's own cache, where it was stored as the
     * fs_vol the manifest was read from; the const is the cache's promise not
     * to touch it, not a promise about the volume. */
    vol = (fs_vol *)(uintptr_t)hit->src;

    result->stage = L"opening /falcon/blessed";
    if (open_blessed_dir(vol, &g_dir_b) != 1) {
        result->advice = L"the blessed directory is gone from this volume. "
                         L"Re-bless the ISO.";
        return EFI_NOT_FOUND;
    }

    result->stage = L"finding the blessed image the manifest names";
    if (dir_find(&g_dir_b, hit->rec->uki, &ent) != 1 || ent.is_dir) {
        result->advice = L"the manifest names an image that is not on the "
                         L"stick. Re-bless the ISO.";
        return EFI_NOT_FOUND;
    }

    menu_screen(st);

    /* Resolved and judged here rather than inside the launch so the warning
     * precedes the read: a blessed wrap carries its own initramfs and the
     * read is the slow part, and telling a user afterwards that the wait was
     * never going to pay off is the wrong order to say it in.
     *
     * The boot proceeds either way. Nothing below reads inner_blocked to
     * decide anything; it only decides what is said (DESIGN Section 7.3). */
    resolve_load_ops(bs, &ops);
    inner_blocked = load_ops_inner_load_blocked(st, &ops);
    if (inner_blocked)
        out(L"warning: this image is very unlikely to start on this machine;"
            L"\r\n         see the note after it exits.\r\n\r\n");

    out(L"reading ");
    out(set_name_ascii(hit->rec->uki));
    out(L" ");
    result->stage = L"reading the blessed image";
    status = read_image(bs, vol, &ent, &buf, &size, &pages);
    if (EFI_ERROR(status)) {
        if (status == EFI_UNSUPPORTED)
            result->advice = L"the blessed image is empty or implausibly "
                             L"large. Re-bless the ISO.";
        return status;
    }
    out(L"\r\n");
    out_dec(size);
    out(L" bytes read.\r\n\r\n");

    /* The blessed wording of the refusal main.c reports for an unblessed ISO:
     * here the image IS ours, so the actionable half is the key, not the
     * blessing. */
    status = launch_image(st, parent_image, buf, size, &ops, inner_blocked,
                          L"the blessed image is not signed with a key this "
                          L"machine has enrolled.\r\n\r\nRe-bless the ISO "
                          L"with the MOK key enrolled here, enroll this "
                          L"stick's\r\ncertificate with MokManager, turn "
                          L"Secure Boot off in firmware setup, or run\r\n"
                          L"mokutil --disable-validation from Linux.",
                          &retain, result);

    /* The image is loaded and running from the firmware's own copy by now, so
     * these pages are ours to release whether it succeeded or not. Doing it
     * here rather than leaking on the failure path is what lets a user retry
     * a refused entry without walking the machine out of memory.
     *
     * The exception is a synthetic filesystem that could not be taken down:
     * something still holds an interface into a filesystem whose one file IS
     * this buffer, so it is leaked on purpose rather than handing a live
     * reader pages the firmware may reissue to somebody else. */
    if (!retain)
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)buf, pages);

    if (EFI_ERROR(status))
        result->status = status;
    return status;
}

/* ---- bare EFI payloads (DESIGN Section 7.2) ---- */

EFI_STATUS bless_payload_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                              fs_vol *vol, const fs_dirent *file,
                              bless_result *result)
{
    EFI_BOOT_SERVICES *bs;
    void *buf = (void *)0;
    UINT64 size = 0;
    UINTN pages = 0;
    UINT8 retain = 0;
    load_ops ops;
    EFI_STATUS status;

    if (st == (EFI_SYSTEM_TABLE *)0 || result == (bless_result *)0)
        return EFI_INVALID_PARAMETER;

    g_st = st;
    bs = st->BootServices;
    result->stage = L"starting";
    result->status = EFI_SUCCESS;
    result->advice = (const CHAR16 *)0;

    if (vol == (fs_vol *)0 || file == (const fs_dirent *)0)
        return EFI_INVALID_PARAMETER;

    menu_screen(st);

    /* No inner-load warning, and FALSE passed for it below rather than the
     * platform's answer. load_ops_inner_load_blocked is about an image that
     * loads a PE of its own through gBS->LoadImage, which is what sd-stub
     * does with its kernel and what a self-contained payload by definition
     * does not do; printing the UKI warning here would be a warning about a
     * mechanism this entry never reaches. */
    resolve_load_ops(bs, &ops);

    out(L"reading ");
    out(set_name_ucs2(file->name));
    out(L" ");
    result->stage = L"reading the payload";
    status = read_image(bs, vol, file, &buf, &size, &pages);
    if (EFI_ERROR(status)) {
        if (status == EFI_UNSUPPORTED)
            result->advice = L"this payload is empty or implausibly large for "
                             L"an EFI binary.";
        return status;
    }
    out(L"\r\n");
    out_dec(size);
    out(L" bytes read.\r\n\r\n");

    /* The refusal wording that fits a payload: the file on p2 IS what the
     * platform verified, so the door is signing that file, which is what
     * blessing a payload does (DESIGN Section 7.2) - and it says so, because
     * that bless is the one that writes to the user's own file. */
    status = launch_image(st, parent_image, buf, size, &ops, FALSE,
                          L"this EFI payload is not signed with a key this "
                          L"machine has enrolled.\r\n\r\nBless it with the "
                          L"sign tool, which rewrites the payload file itself; "
                          L"or turn\r\nSecure Boot off in firmware setup, or "
                          L"run mokutil --disable-validation\r\nfrom Linux.",
                          &retain, result);

    /* Same ownership rule as the blessed path: the firmware has its own copy
     * by now, so the pages go back unless a filesystem that serves them could
     * not be taken down. */
    if (!retain)
        bs->FreePages((EFI_PHYSICAL_ADDRESS)(uintptr_t)buf, pages);

    if (EFI_ERROR(status))
        result->status = status;
    return status;
}

/* ---- prepared media (DESIGN Sections 6.3 and 6.3.1) ---- */

/* Opens every sidecar the override set names, in one pass over the prepared
 * folder. Returns 1 with each entry's src filled in, or 0 having written the
 * name of the first one that is not there into *missing.
 *
 * One pass rather than one walk per entry because the set is at most
 * ISO_MAX_OVERRIDES long and a directory walk is a whole FAT chain descent:
 * matching four names against each dirent as it goes costs nothing and reads
 * the directory once. A name that matches a subdirectory is not a match, for
 * the same reason a manifest naming a directory as its UKI is not one. */
static int open_sidecars(fs_vol *vol, fs_dir *dir, const manifest_record *r,
                         iso_overlay *ovl, fs_file *files,
                         const char **missing)
{
    fs_dirent ent;
    UINTN opened = 0;
    UINTN i;

    *missing = (const char *)0;
    while (fs_dir_next(dir, &ent) == 1) {
        if (ent.is_dir)
            continue;
        for (i = 0; i < ovl->n; i++) {
            if (ovl->ovl[i].src != (fs_file *)0)
                continue;
            if (!fs_name_eq_ascii(ent.name, r->splice[i].name))
                continue;
            if (fs_file_open(vol, &files[i], &ent) != FS_OK) {
                *missing = r->splice[i].name;
                return 0;
            }
            ovl->ovl[i].src = &files[i];
            opened++;
        }
        if (opened == ovl->n)
            break;
    }

    for (i = 0; i < ovl->n; i++) {
        if (ovl->ovl[i].src == (fs_file *)0) {
            *missing = r->splice[i].name;
            return 0;
        }
    }
    return 1;
}

/* The line to print when the byte map cannot be built or cannot be honoured.
 * Every one of them ends in the same place - the preparation on the stick does
 * not match the ISO beside it - so every one of them names the same remedy. */
static const CHAR16 *prepared_advice(int rc)
{
    switch (rc) {
    case BLESS_ERR_NOSPLICE:
        return L"this ISO's record carries no byte ranges, so preparing it "
               L"did nothing.\r\nPrepare it again, or delete the record.";
    case BLESS_ERR_SPLICES:
        return L"this ISO's record asks for more byte ranges than a medium "
               L"may carry.\r\nPrepare the ISO again.";
    case BLESS_ERR_KIND:
    default:
        return L"this ISO's record does not describe prepared media. Prepare "
               L"the ISO again.";
    }
}

EFI_STATUS bless_prepared_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                               const bless_hit *hit, fs_vol *vol,
                               const fs_dirent *file, iso_media_kind kind,
                               bless_result *result)
{
    EFI_BOOT_SERVICES *bs;
    iso_boot_result iso_result = { L"starting", EFI_SUCCESS,
                                   (const family_desc *)0 };
    iso_overlay ovl;
    const manifest_record *rec;
    const char *missing = (const char *)0;
    fs_file *files = (fs_file *)0;
    EFI_STATUS status;
    int rc;

    if (st == (EFI_SYSTEM_TABLE *)0 || result == (bless_result *)0)
        return EFI_INVALID_PARAMETER;

    g_st = st;
    bs = st->BootServices;
    result->stage = L"starting";
    result->status = EFI_SUCCESS;
    result->advice = (const CHAR16 *)0;

    if (hit == (const bless_hit *)0 ||
        hit->rec == (const manifest_record *)0 || vol == (fs_vol *)0 ||
        file == (const fs_dirent *)0)
        return EFI_INVALID_PARAMETER;
    rec = hit->rec;

    result->stage = L"reading the prepared record";
    rc = bless_prepared_overlay(rec, &ovl);
    if (rc != BLESS_OK) {
        result->advice = prepared_advice(rc);
        return EFI_UNSUPPORTED;
    }
    /* The folder is named by the record's own hash, so a record that lost its
     * sha256= cannot say where its artifacts are - which manifest_parse also
     * refuses to write down, making this the belt to that braces. */
    if (rec->has_sha256 == 0) {
        result->stage = L"locating the prepared artifacts";
        result->advice = L"this ISO's record does not say where its prepared "
                         L"artifacts are.\r\nPrepare the ISO again.";
        return EFI_UNSUPPORTED;
    }

    result->stage = L"opening /falcon/prepared";
    if (open_prepared_dir(vol, rec->sha256, &g_dir_b) != 1) {
        result->advice = L"the prepared artifacts for this ISO are not on the "
                         L"stick.\r\nPrepare it again, or boot it unprepared "
                         L"knowing Setup will not find\r\nits install source.";
        return EFI_NOT_FOUND;
    }

    /* Pool rather than static storage: one file handle carries a multi-kilo-
     * byte driver cache, and four of them are live only for the length of one
     * boot attempt. They must outlast the attempt itself, though - the virtual
     * medium reads through them on every block - so they are released after
     * iso_boot_overlaid has returned and torn its device down. */
    status = bs->AllocatePool(EfiLoaderData,
                              (UINTN)(ISO_MAX_OVERRIDES * sizeof(fs_file)),
                              (void **)&files);
    if (EFI_ERROR(status)) {
        result->stage = L"making room for the prepared sidecars";
        return status;
    }

    result->stage = L"opening the prepared sidecars";
    if (!open_sidecars(vol, &g_dir_b, rec, &ovl, files, &missing)) {
        CHAR16 name[MANIFEST_SPLICE_NAME_MAX];

        bs->FreePool(files);
        ascii_to_ucs2(name, MANIFEST_SPLICE_NAME_MAX,
                      (missing != (const char *)0) ? missing : "?");
        out(L"missing prepared file: ");
        out(name);
        out(L"\r\n");
        result->advice = L"a file this ISO's preparation depends on is "
                         L"missing from the stick, so\r\nthe ISO was not "
                         L"booted. Prepare it again.";
        return EFI_NOT_FOUND;
    }

    menu_screen(st);
    out(L"booting prepared media; ");
    out_dec(ovl.n);
    out(L" byte range(s) served from the stick.\r\n\r\n");

    status = iso_boot_overlaid(st, parent_image, vol, file, kind, &ovl,
                               &iso_result);
    result->stage = iso_result.stage;
    result->status = iso_result.status;
    bs->FreePool(files);

    /* The two statuses the override gate answers with (src/iso/iso.c), as
     * opposed to a failure further along that has nothing to do with the byte
     * map. The stage string already names which check refused it; this says
     * what to do about any of them. */
    if (status == EFI_INVALID_PARAMETER || status == EFI_UNSUPPORTED)
        result->advice = L"the byte map staged for this ISO does not fit the "
                         L"ISO beside it, so\r\nnothing was served. Prepare "
                         L"the ISO again.";
    if (EFI_ERROR(status))
        result->status = status;
    return status;
}
