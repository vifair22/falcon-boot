/* The synthetic credential filesystem. See falcon-efi/src/loader/credfs.h for
 * the contract and docs/DESIGN.md Section 5.1 for why the personal-OS path
 * needs a device to load from at all.
 *
 * Shape of the thing: a fixed array of entries describing a tree, a pool of
 * open handles, one Simple File System instance and one device path. There is
 * no allocation and no I/O; every Read is a memcpy out of a buffer the caller
 * already owns. That is deliberate. This code runs as a firmware callback,
 * possibly from inside LoadImage, and the less it can fail at the worse
 * moment the better.
 */

#include <loader/credfs.h>
#include <loader/credfs_efi.h>

#include <crypto/crypto.h>
#include <efi/devpath.h>
#include <efi/sfs.h>
#include <lib/string.h>

/* Identifies our synthetic filesystem in device paths. A vendor-defined media
 * node is the spec's answer for a device with no bus behind it; the ISO path
 * uses the same shape with a GUID of its own. */
#define FALCON_CREDFS_MEDIA_GUID \
    { 0x2f1c9a64, 0x8d3e, 0x4a17, \
      { 0xb0, 0x5c, 0x9e, 0x41, 0x77, 0x2d, 0x6b, 0x8a } }

/* Concurrently open File Protocol handles. The firmware's LoadImage holds one
 * (root) plus one for the image; sd-stub holds root, the drop-in directory,
 * and one credential at a time. Eight is slack over both. */
#define CREDFS_MAX_OPEN 8u

/* Label the volume reports if anything asks. Cosmetic: nothing in the boot
 * path matches on it. */
static const CHAR16 credfs_label[] = L"FALCON-CRED";

/* What GetInfo calls the root directory. FAT drivers answer with the path
 * separator; no consumer in this loader's chain reads it. */
static const CHAR16 credfs_root_name[] = L"\\";

typedef struct {
    VENDOR_DEVICE_PATH       vendor;
    EFI_DEVICE_PATH_PROTOCOL end;
} credfs_device_path;

/* Device path nodes are byte-packed; a compiler that padded this struct would
 * publish a path the firmware cannot walk. */
_Static_assert(sizeof(credfs_device_path) ==
                   sizeof(VENDOR_DEVICE_PATH) +
                       sizeof(EFI_DEVICE_PATH_PROTOCOL),
               "device path nodes must pack without padding");

typedef struct {
    /* First member on purpose: the interface pointer the firmware hands back
     * to our callbacks is the address of the instance. */
    CREDFS_EFI_FILE_PROTOCOL file;
    UINTN                    entry;   /* index, or CREDFS_ROOT for the root */
    /* Byte offset for a file; index of the next child to report for a
     * directory. The two never share a handle, so they share a field. */
    UINT64                   pos;
    UINT8                    open;
} credfs_handle;

/* One filesystem at a time, and the instance must outlive every firmware
 * caller that holds a pointer into it, so the state is static rather than
 * pool-allocated. */
static struct {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL sfs;
    credfs_device_path              path;
    credfs_entry                    entries[CREDFS_MAX_ENTRIES];
    UINTN                           entry_n;
    credfs_handle                   handles[CREDFS_MAX_OPEN];
    EFI_HANDLE                      handle;
    UINT8                           in_use;
} g_fs;

/* ---- tree helpers ---- */

static int ascii_lower(CHAR16 c)
{
    return (c >= L'A' && c <= L'Z') ? (int)c + (L'a' - L'A') : (int)c;
}

/* Does entry `i`'s name equal the `len`-char component at `comp`? FAT
 * semantics: ASCII case folding, which is all the names on this filesystem can
 * contain (falcon-efi/src/pos builds them from an ext4 listing it has already
 * narrowed to printable ASCII). */
static BOOLEAN name_eq(UINTN i, const CHAR16 *comp, UINTN len)
{
    const CHAR16 *name = g_fs.entries[i].name;
    UINTN k;

    for (k = 0; k < len; k++) {
        if (name[k] == 0 || ascii_lower(name[k]) != ascii_lower(comp[k]))
            return FALSE;
    }
    return (BOOLEAN)(name[len] == 0);
}

/* The k-th child of `dir`, or CREDFS_ROOT when there is no such child. The
 * scan is linear over a tree of three entries; an index would be more code
 * than the loop it replaced. */
static UINTN child_at(UINTN dir, UINT64 k)
{
    UINT64 seen = 0;
    UINTN i;

    for (i = 0; i < g_fs.entry_n; i++) {
        if (g_fs.entries[i].parent != dir)
            continue;
        if (seen == k)
            return i;
        seen++;
    }
    return CREDFS_ROOT;
}

static BOOLEAN entry_is_dir(UINTN i)
{
    return (BOOLEAN)(i == CREDFS_ROOT || g_fs.entries[i].is_dir != 0);
}

/* Walks a File Protocol path from `from`. Both separators are accepted: the
 * spec says backslash and that is what every caller in the boot chain emits,
 * but a forward slash names nothing else here and refusing it would only
 * convert a working open into a mystery. */
static EFI_STATUS resolve(UINTN from, const CHAR16 *path, UINTN *out)
{
    UINTN cur = from;
    UINTN i = 0;

    if (path == (const CHAR16 *)0)
        return EFI_INVALID_PARAMETER;
    if (path[0] == L'\\' || path[0] == L'/') {
        cur = CREDFS_ROOT;
        i = 1;
    }

    while (path[i] != 0) {
        UINTN start = i;
        UINTN len;
        UINTN k;
        UINTN found = CREDFS_ROOT;
        BOOLEAN matched = FALSE;

        while (path[i] != 0 && path[i] != L'\\' && path[i] != L'/')
            i++;
        len = i - start;
        if (path[i] != 0)
            i++;

        /* An empty component names the directory it sits in, which is what
         * makes a trailing or doubled separator harmless. */
        if (len == 0)
            continue;
        if (len == 1 && path[start] == L'.')
            continue;
        if (len == 2 && path[start] == L'.' && path[start + 1] == L'.') {
            cur = (cur == CREDFS_ROOT) ? CREDFS_ROOT : g_fs.entries[cur].parent;
            continue;
        }

        /* Only a directory has children, so a path that walks through a file
         * is not found rather than malformed. */
        if (!entry_is_dir(cur))
            return EFI_NOT_FOUND;
        for (k = 0; k < g_fs.entry_n; k++) {
            if (g_fs.entries[k].parent != cur)
                continue;
            if (!name_eq(k, path + start, len))
                continue;
            found = k;
            matched = TRUE;
            break;
        }
        if (!matched)
            return EFI_NOT_FOUND;
        cur = found;
    }

    *out = cur;
    return EFI_SUCCESS;
}

/* ---- handle pool ---- */

static void file_init(CREDFS_EFI_FILE_PROTOCOL *f);

static credfs_handle *handle_alloc(UINTN entry)
{
    UINTN i;

    for (i = 0; i < CREDFS_MAX_OPEN; i++) {
        credfs_handle *h = &g_fs.handles[i];

        if (h->open)
            continue;
        file_init(&h->file);
        h->entry = entry;
        h->pos = 0;
        h->open = 1;
        return h;
    }
    return (credfs_handle *)0;
}

static credfs_handle *handle_of(CREDFS_EFI_FILE_PROTOCOL *self)
{
    return (credfs_handle *)(void *)self;
}

/* ---- the published File Protocol ---- */

static EFI_STATUS EFIAPI credfs_open(CREDFS_EFI_FILE_PROTOCOL *self,
                                     CREDFS_EFI_FILE_PROTOCOL **new_handle,
                                     CHAR16 *name, UINT64 mode,
                                     UINT64 attributes)
{
    credfs_handle *h = handle_of(self);
    credfs_handle *child;
    UINTN target = CREDFS_ROOT;
    EFI_STATUS status;

    (void)attributes;
    if (new_handle == (CREDFS_EFI_FILE_PROTOCOL **)0)
        return EFI_INVALID_PARAMETER;
    if (!h->open)
        return EFI_INVALID_PARAMETER;
    /* Read-only media: the spec's answer to an open for write, and to a
     * create, is EFI_WRITE_PROTECTED. Saying so is not a policy decision,
     * it is the honest capability of a filesystem that lives in RAM for the
     * length of one LoadImage. */
    if ((mode & (EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE)) != 0)
        return EFI_WRITE_PROTECTED;
    if ((mode & EFI_FILE_MODE_READ) == 0)
        return EFI_INVALID_PARAMETER;

    status = resolve(h->entry, name, &target);
    if (EFI_ERROR(status))
        return status;

    child = handle_alloc(target);
    if (child == (credfs_handle *)0)
        return EFI_OUT_OF_RESOURCES;
    *new_handle = &child->file;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI credfs_close(CREDFS_EFI_FILE_PROTOCOL *self)
{
    handle_of(self)->open = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI credfs_delete(CREDFS_EFI_FILE_PROTOCOL *self)
{
    /* Spec 13.5.4: the handle is closed either way, and a delete that did not
     * happen is reported as a warning rather than an error. */
    handle_of(self)->open = 0;
    return EFI_WARN_DELETE_FAILURE;
}

/* Fills an EFI_FILE_INFO for `entry` (CREDFS_ROOT for the root directory).
 * Returns EFI_BUFFER_TOO_SMALL with the required size in *size when the
 * buffer does not fit the record, which is the protocol's way of asking the
 * caller to allocate; both GetInfo and directory Read need it. */
static EFI_STATUS fill_file_info(UINTN entry, UINTN *size, void *buf)
{
    const CHAR16 *name = (entry == CREDFS_ROOT)
                             ? credfs_root_name
                             : g_fs.entries[entry].name;
    UINTN name_bytes = (ucs2_len(name) + 1) * sizeof(CHAR16);
    UINTN need = SIZE_OF_EFI_FILE_INFO + name_bytes;
    EFI_FILE_INFO *info;

    if (*size < need || buf == (void *)0) {
        *size = need;
        return EFI_BUFFER_TOO_SMALL;
    }

    info = (EFI_FILE_INFO *)buf;
    memset(info, 0, SIZE_OF_EFI_FILE_INFO);
    info->Size = need;
    info->FileSize = (entry == CREDFS_ROOT) ? 0 : g_fs.entries[entry].size;
    info->PhysicalSize = info->FileSize;
    /* The three timestamps stay zeroed. This filesystem has no clock and no
     * history; inventing one would be data, not information. */
    info->Attribute = EFI_FILE_READ_ONLY;
    if (entry_is_dir(entry))
        info->Attribute |= EFI_FILE_DIRECTORY;
    memcpy(info->FileName, name, name_bytes);

    *size = need;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI credfs_read(CREDFS_EFI_FILE_PROTOCOL *self,
                                     UINTN *size, void *buf)
{
    credfs_handle *h = handle_of(self);
    const credfs_entry *e;
    UINT64 left;
    UINTN n;

    if (size == (UINTN *)0)
        return EFI_INVALID_PARAMETER;
    if (!h->open)
        return EFI_DEVICE_ERROR;

    /* A directory read produces one EFI_FILE_INFO per call and a zero-length
     * answer at the end. This is the call sd-stub's readdir() is built on, so
     * the end-of-directory shape matters as much as the records do: the
     * convention is EFI_SUCCESS with *size set to 0, never EFI_NOT_FOUND. */
    if (entry_is_dir(h->entry)) {
        UINTN child = child_at(h->entry, h->pos);
        EFI_STATUS status;

        if (child == CREDFS_ROOT) {
            *size = 0;
            return EFI_SUCCESS;
        }
        status = fill_file_info(child, size, buf);
        /* The position advances only on a delivered record, so the retry a
         * EFI_BUFFER_TOO_SMALL asks for returns the same entry. */
        if (status == EFI_SUCCESS)
            h->pos++;
        return status;
    }

    if (buf == (void *)0 && *size != 0)
        return EFI_INVALID_PARAMETER;

    e = &g_fs.entries[h->entry];
    left = (h->pos < e->size) ? e->size - h->pos : 0;
    n = (*size < left) ? *size : (UINTN)left;
    if (n != 0)
        memcpy(buf, (const UINT8 *)e->data + h->pos, n);
    h->pos += n;
    *size = n;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI credfs_write(CREDFS_EFI_FILE_PROTOCOL *self,
                                      UINTN *size, void *buf)
{
    (void)self;
    (void)size;
    (void)buf;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI credfs_get_position(CREDFS_EFI_FILE_PROTOCOL *self,
                                             UINT64 *position)
{
    credfs_handle *h = handle_of(self);

    if (position == (UINT64 *)0)
        return EFI_INVALID_PARAMETER;
    /* Spec 13.5.12: a directory's position has no meaning outside the driver,
     * and the call is defined to be unsupported on one. */
    if (entry_is_dir(h->entry))
        return EFI_UNSUPPORTED;
    *position = h->pos;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI credfs_set_position(CREDFS_EFI_FILE_PROTOCOL *self,
                                             UINT64 position)
{
    credfs_handle *h = handle_of(self);

    /* Spec 13.5.13: on a directory only zero is legal, and it restarts the
     * enumeration. */
    if (entry_is_dir(h->entry)) {
        if (position != 0)
            return EFI_UNSUPPORTED;
        h->pos = 0;
        return EFI_SUCCESS;
    }
    /* All-ones means "seek to the end"; any other value past the end is
     * allowed and simply makes the next read deliver nothing. */
    h->pos = (position == 0xFFFFFFFFFFFFFFFFULL) ? g_fs.entries[h->entry].size
                                                 : position;
    return EFI_SUCCESS;
}

static BOOLEAN guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    return (BOOLEAN)(memcmp(a, b, sizeof(EFI_GUID)) == 0);
}

static EFI_STATUS EFIAPI credfs_get_info(CREDFS_EFI_FILE_PROTOCOL *self,
                                         EFI_GUID *type, UINTN *size,
                                         void *buf)
{
    EFI_GUID file_info = EFI_FILE_INFO_GUID;
    EFI_GUID fs_info = EFI_FILE_SYSTEM_INFO_ID_GUID;
    EFI_GUID fs_label = EFI_FILE_SYSTEM_VOLUME_LABEL_ID_GUID;
    UINTN label_bytes = (ucs2_len(credfs_label) + 1) * sizeof(CHAR16);
    UINTN need;

    if (type == (EFI_GUID *)0 || size == (UINTN *)0)
        return EFI_INVALID_PARAMETER;

    if (guid_eq(type, &file_info))
        return fill_file_info(handle_of(self)->entry, size, buf);

    if (guid_eq(type, &fs_info)) {
        EFI_FILE_SYSTEM_INFO *info;

        need = SIZE_OF_EFI_FILE_SYSTEM_INFO + label_bytes;
        if (*size < need || buf == (void *)0) {
            *size = need;
            return EFI_BUFFER_TOO_SMALL;
        }
        info = (EFI_FILE_SYSTEM_INFO *)buf;
        memset(info, 0, SIZE_OF_EFI_FILE_SYSTEM_INFO);
        info->Size = need;
        info->ReadOnly = TRUE;
        /* There is no medium to report the size of, and no free space on a
         * filesystem nothing can write to. A one-byte block keeps any
         * arithmetic a caller does on these honest rather than rounded. */
        info->VolumeSize = 0;
        info->FreeSpace = 0;
        info->BlockSize = 1;
        memcpy(info->VolumeLabel, credfs_label, label_bytes);
        *size = need;
        return EFI_SUCCESS;
    }

    if (guid_eq(type, &fs_label)) {
        need = SIZE_OF_EFI_FILE_SYSTEM_VOLUME_LABEL + label_bytes;
        if (*size < need || buf == (void *)0) {
            *size = need;
            return EFI_BUFFER_TOO_SMALL;
        }
        memcpy(buf, credfs_label, label_bytes);
        *size = need;
        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI credfs_set_info(CREDFS_EFI_FILE_PROTOCOL *self,
                                         EFI_GUID *type, UINTN size,
                                         void *buf)
{
    (void)self;
    (void)type;
    (void)size;
    (void)buf;
    return EFI_WRITE_PROTECTED;
}

static EFI_STATUS EFIAPI credfs_flush(CREDFS_EFI_FILE_PROTOCOL *self)
{
    (void)self;
    return EFI_WRITE_PROTECTED;
}

static void file_init(CREDFS_EFI_FILE_PROTOCOL *f)
{
    f->Revision = EFI_FILE_PROTOCOL_REVISION;
    f->Open = credfs_open;
    f->Close = credfs_close;
    f->Delete = credfs_delete;
    f->Read = credfs_read;
    f->Write = credfs_write;
    f->GetPosition = credfs_get_position;
    f->SetPosition = credfs_set_position;
    f->GetInfo = credfs_get_info;
    f->SetInfo = credfs_set_info;
    f->Flush = credfs_flush;
}

/* ---- the published Simple File System instance ---- */

static EFI_STATUS EFIAPI credfs_open_volume(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self, EFI_FILE_PROTOCOL **root)
{
    credfs_handle *h;

    (void)self;
    if (root == (EFI_FILE_PROTOCOL **)0)
        return EFI_INVALID_PARAMETER;
    if (!g_fs.in_use)
        return EFI_NO_MEDIA;

    h = handle_alloc(CREDFS_ROOT);
    if (h == (credfs_handle *)0)
        return EFI_OUT_OF_RESOURCES;
    /* The two views of the File Protocol are the same object; credfs_efi.h
     * asserts that, so this cast moves no bytes. */
    *root = (EFI_FILE_PROTOCOL *)(void *)&h->file;
    return EFI_SUCCESS;
}

/* ---- lifecycle ---- */

static BOOLEAN tree_is_sane(const credfs_entry *entries, UINTN count)
{
    UINTN i;

    for (i = 0; i < count; i++) {
        const credfs_entry *e = &entries[i];

        if (e->name == (const CHAR16 *)0 || e->name[0] == 0)
            return FALSE;
        /* A parent must be an EARLIER directory. Earlier is what makes the
         * tree acyclic by construction, so no walk here or in resolve() can
         * loop on a caller's mistake. */
        if (e->parent != CREDFS_ROOT) {
            if (e->parent >= i || !entries[e->parent].is_dir)
                return FALSE;
        }
        if (e->is_dir) {
            if (e->data != (void *)0 || e->size != 0)
                return FALSE;
        } else if (e->data == (void *)0 && e->size != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

EFI_STATUS credfs_publish(EFI_SYSTEM_TABLE *st, const credfs_entry *entries,
                          UINTN count)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID vendor_guid = FALCON_CREDFS_MEDIA_GUID;
    EFI_STATUS status;
    UINTN i;

    if (st == (EFI_SYSTEM_TABLE *)0 || entries == (const credfs_entry *)0)
        return EFI_INVALID_PARAMETER;
    if (count == 0 || count > CREDFS_MAX_ENTRIES)
        return EFI_INVALID_PARAMETER;
    if (g_fs.in_use)
        return EFI_ACCESS_DENIED;
    if (!tree_is_sane(entries, count))
        return EFI_INVALID_PARAMETER;

    /* The table is copied so the caller may build it on its stack; the file
     * DATA is not, and stays the caller's (see credfs.h). */
    for (i = 0; i < count; i++)
        g_fs.entries[i] = entries[i];
    g_fs.entry_n = count;
    memset(g_fs.handles, 0, sizeof g_fs.handles);

    g_fs.sfs.Revision = EFI_FILE_SYSTEM_PROTOCOL_REVISION;
    g_fs.sfs.OpenVolume = credfs_open_volume;

    g_fs.path.vendor.Header.Type = MEDIA_DEVICE_PATH;
    g_fs.path.vendor.Header.SubType = MEDIA_VENDOR_DP;
    dp_set_node_len(&g_fs.path.vendor.Header, (UINT16)sizeof(g_fs.path.vendor));
    g_fs.path.vendor.Guid = vendor_guid;
    dp_set_end(&g_fs.path.end);

    g_fs.handle = (EFI_HANDLE)0;
    /* Set before the install: OpenVolume refuses while it is clear, and the
     * firmware is entitled to call into us the moment the interface lands. */
    g_fs.in_use = 1;
    status = st->BootServices->InstallMultipleProtocolInterfaces(
        &g_fs.handle,
        &sfs_guid, &g_fs.sfs,
        &dp_guid, &g_fs.path,
        (void *)0);
    if (EFI_ERROR(status)) {
        g_fs.in_use = 0;
        g_fs.entry_n = 0;
        return status;
    }
    return EFI_SUCCESS;
}

EFI_STATUS credfs_load_path(EFI_BOOT_SERVICES *bs, const CHAR16 *path,
                            EFI_DEVICE_PATH_PROTOCOL **out)
{
    UINTN end_size = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN name_bytes;
    UINTN node_len;
    UINTN total;
    UINT8 *buf = (UINT8 *)0;
    EFI_DEVICE_PATH_PROTOCOL *node;
    EFI_STATUS status;

    if (bs == (EFI_BOOT_SERVICES *)0 || path == (const CHAR16 *)0 ||
        out == (EFI_DEVICE_PATH_PROTOCOL **)0)
        return EFI_INVALID_PARAMETER;
    if (!g_fs.in_use)
        return EFI_NOT_READY;

    name_bytes = (ucs2_len(path) + 1) * sizeof(CHAR16);
    node_len = end_size + name_bytes;
    if (node_len > 0xFFFFu)
        return EFI_INVALID_PARAMETER;
    total = sizeof(g_fs.path.vendor) + node_len + end_size;

    status = bs->AllocatePool(EfiLoaderData, total, (void **)&buf);
    if (EFI_ERROR(status))
        return status;

    memcpy(buf, &g_fs.path.vendor, sizeof(g_fs.path.vendor));

    node = (EFI_DEVICE_PATH_PROTOCOL *)(void *)(buf + sizeof(g_fs.path.vendor));
    node->Type = MEDIA_DEVICE_PATH;
    node->SubType = MEDIA_FILEPATH_DP;
    dp_set_node_len(node, (UINT16)node_len);
    memcpy(buf + sizeof(g_fs.path.vendor) + end_size, path, name_bytes);

    dp_set_end((EFI_DEVICE_PATH_PROTOCOL *)(void *)(
        buf + sizeof(g_fs.path.vendor) + node_len));

    *out = (EFI_DEVICE_PATH_PROTOCOL *)(void *)buf;
    return EFI_SUCCESS;
}

EFI_STATUS credfs_teardown(EFI_BOOT_SERVICES *bs)
{
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_STATUS status;
    UINTN i;

    if (bs == (EFI_BOOT_SERVICES *)0)
        return EFI_INVALID_PARAMETER;
    if (!g_fs.in_use)
        return EFI_SUCCESS;

    /* No DisconnectController counterpart to the ISO path's: nothing binds to
     * a Simple File System handle. This module IS the driver, and the only
     * users are callers holding File Protocol pointers, which the uninstall
     * below is what stops them acquiring more of. */
    status = bs->UninstallMultipleProtocolInterfaces(
        g_fs.handle,
        &sfs_guid, &g_fs.sfs,
        &dp_guid, &g_fs.path,
        (void *)0);
    if (EFI_ERROR(status)) {
        /* Somebody still holds an interface. The state is static so it stays
         * valid, but it must never back a second filesystem: leaving in_use
         * set is what refuses that, and the secret is NOT wiped here, because
         * a live reader would then serve zeroes out of a buffer it believes
         * still holds the credential. */
        return status;
    }

    /* One wipe site for the credential copy, on every path out of the launch
     * (DESIGN Section 5.1). */
    for (i = 0; i < g_fs.entry_n; i++) {
        if (g_fs.entries[i].secret && g_fs.entries[i].data != (void *)0)
            crypto_wipe(g_fs.entries[i].data, (size_t)g_fs.entries[i].size);
    }

    memset(g_fs.handles, 0, sizeof g_fs.handles);
    memset(g_fs.entries, 0, sizeof g_fs.entries);
    g_fs.entry_n = 0;
    g_fs.handle = (EFI_HANDLE)0;
    g_fs.in_use = 0;
    return EFI_SUCCESS;
}
