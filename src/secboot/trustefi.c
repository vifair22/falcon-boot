#include <secboot/trustefi.h>

#include <efi/loadedimage.h>
#include <efi/sfs.h>
#include <lib/string.h>

/* d719b2cb-3d3a-4596-a3bc-dad00e67656f: db and dbx */
#define IMAGE_SECURITY_DATABASE_GUID \
    { 0xd719b2cb, 0x3d3a, 0x4596, \
      { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f } }

/* 605dab50-e046-4300-abb6-3dd810dd8b23: shim's runtime mirrors */
#define SHIM_LOCK_GUID \
    { 0x605dab50, 0xe046, 0x4300, \
      { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 } }

/* A sanity bound per variable, not a format limit. A 2026 dbx is ~25 KiB; a
 * store past this is one this code declines to copy, as if unreadable. */
#define VAR_MAX     ((UINTN)1u << 20)

/* Shim splits a MokList too large for one variable across MokListRT,
 * MokListRT1, MokListRT2, ... (shim's mok.c, "mirror_one_mok_variable").
 * Nine parts is far past anything MokManager produces. */
#define PARTS_MAX   9u

#define CERT_MAX    4096u

/* The certificate mkstick writes to p1 for enrollment
 * (src/cli/mkstick/mkstick.h MKS_ESP_CERT_NAME). */
#define OWN_CERT_PATH L"\\falcon-boot.cer"

typedef enum { VAR_ABSENT = 0, VAR_READ, VAR_FAILED } var_status;

/* Size of one variable, without reading it. */
static var_status var_size(EFI_RUNTIME_SERVICES *rt, const CHAR16 *name,
                           EFI_GUID *guid, UINTN *size)
{
    EFI_STATUS s;

    *size = 0;
    s = rt->GetVariable((CHAR16 *)(uintptr_t)name, guid, (UINT32 *)0, size,
                        (void *)0);
    if (s == EFI_NOT_FOUND)
        return VAR_ABSENT;
    if (s != EFI_BUFFER_TOO_SMALL || *size == 0 || *size > VAR_MAX)
        return VAR_FAILED;
    return VAR_READ;
}

/* Reads `base`, then base1, base2, ... while they exist, concatenated into
 * one pool allocation: a sequence of EFI_SIGNATURE_LISTs split at list
 * boundaries is still one when rejoined. A single-name store is the same call
 * with parts = 1. */
static var_status read_store(EFI_SYSTEM_TABLE *st, const CHAR16 *base,
                             EFI_GUID *guid, UINTN parts, der_span *out)
{
    EFI_RUNTIME_SERVICES *rt = st->RuntimeServices;
    CHAR16 name[24];
    UINTN sizes[PARTS_MAX];
    UINTN n, i, total = 0, at = 0, blen;
    uint8_t *buf;

    out->p = (const uint8_t *)0;
    out->len = 0;
    if (rt == (EFI_RUNTIME_SERVICES *)0 || rt->GetVariable == (void *)0)
        return VAR_FAILED;

    /* Room for the base, one digit and the terminator. */
    blen = ucs2_len(base);
    if (blen + 2u > sizeof(name) / sizeof(name[0]))
        return VAR_FAILED;
    memcpy(name, base, blen * sizeof(CHAR16));

    for (n = 0; n < parts && n < PARTS_MAX; n++) {
        var_status vs;

        if (n == 0) {
            name[blen] = 0;
        } else {
            name[blen] = (CHAR16)(u'0' + n);
            name[blen + 1u] = 0;
        }
        vs = var_size(rt, name, guid, &sizes[n]);
        if (vs == VAR_ABSENT)
            break;
        if (vs == VAR_FAILED || sizes[n] > VAR_MAX - total)
            return VAR_FAILED;
        total += sizes[n];
    }
    if (n == 0)
        return VAR_ABSENT;

    if (st->BootServices->AllocatePool(EfiLoaderData, total,
                                       (void **)&buf) != EFI_SUCCESS)
        return VAR_FAILED;
    for (i = 0; i < n; i++) {
        UINTN got = sizes[i];

        if (i == 0) {
            name[blen] = 0;
        } else {
            name[blen] = (CHAR16)(u'0' + i);
            name[blen + 1u] = 0;
        }
        /* A variable that changed size between the two calls is one this
         * run cannot read consistently; nothing at boot services time
         * should do that. */
        if (rt->GetVariable(name, guid, (UINT32 *)0, &got, buf + at) !=
                EFI_SUCCESS ||
            got != sizes[i]) {
            st->BootServices->FreePool(buf);
            return VAR_FAILED;
        }
        at += got;
    }
    out->p = buf;
    out->len = total;
    return VAR_READ;
}

/* The stick's own MOK certificate, from p1. Anything unusual (no loaded
 * image, no filesystem, no file, a file too large to be a certificate) leaves
 * the span empty, which costs the word "MOK" on the screen and nothing else. */
static void read_own_mok(EFI_SYSTEM_TABLE *st, EFI_HANDLE image,
                         der_span *out)
{
    static uint8_t cert[CERT_MAX];
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_FILE_PROTOCOL *root, *file;
    UINTN got = sizeof(cert);
    EFI_STATUS s;

    out->p = (const uint8_t *)0;
    out->len = 0;
    if (bs->HandleProtocol(image, &li_guid, (void **)&li) != EFI_SUCCESS ||
        bs->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&sfs) !=
            EFI_SUCCESS ||
        sfs->OpenVolume(sfs, &root) != EFI_SUCCESS)
        return;
    s = root->Open(root, &file, (CHAR16 *)(uintptr_t)OWN_CERT_PATH,
                   EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (s != EFI_SUCCESS)
        return;
    s = file->Read(file, &got, cert);
    file->Close(file);
    /* A full buffer means the file may be longer than we read. */
    if (s != EFI_SUCCESS || got == 0 || got >= sizeof(cert))
        return;
    out->p = cert;
    out->len = got;
}

void trust_stores_load(EFI_SYSTEM_TABLE *st, EFI_HANDLE image,
                       trust_stores *out)
{
    EFI_GUID secdb = IMAGE_SECURITY_DATABASE_GUID;
    EFI_GUID shim = SHIM_LOCK_GUID;

    if (out == (trust_stores *)0)
        return;
    memset(out, 0, sizeof(*out));
    if (st == (EFI_SYSTEM_TABLE *)0)
        return;

    /* The allow lists decide whether "untrusted" can be said at all. db
     * absent under Secure Boot is not a real platform, so absent counts with
     * unreadable; MokListRT absent just means nothing is enrolled. The deny
     * lists and SBAT only ever ADD a refusal, so failing to read one costs a
     * possible REVOKED and makes nothing else uncertain. */
    if (read_store(st, L"db", &secdb, 1, &out->db) != VAR_READ)
        out->incomplete = 1;
    if (read_store(st, L"MokListRT", &shim, PARTS_MAX, &out->mok) ==
        VAR_FAILED)
        out->incomplete = 1;
    (void)read_store(st, L"dbx", &secdb, 1, &out->dbx);
    (void)read_store(st, L"MokListXRT", &shim, PARTS_MAX, &out->mokx);
    (void)read_store(st, L"SbatLevelRT", &shim, 1, &out->sbat_level);
    read_own_mok(st, image, &out->own_mok);
}
