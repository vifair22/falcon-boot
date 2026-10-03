/* falcon-boot: systemd's Boot Loader Interface, the timing half. See bli.h. */

#include <loader/bli.h>
#include <loader/loadops.h>
#include <tsc/tsccore.h>

/* The longest FirmwareVendor written, in units. A longer one leaves
 * LoaderFirmwareInfo unset rather than cut, so what is there is what the
 * firmware said. */
#define BLI_VENDOR_MAX 192u
#define BLI_STR_MAX (BLI_VENDOR_MAX + 32u)

static UINTN ulen(const CHAR16 *s)
{
    UINTN n = 0;

    while (s[n] != 0)
        n++;
    return n;
}

/* The string with its terminator, as systemd-boot's efivar_set_str16
 * writes it (strsize16). */
static EFI_STATUS set_str(EFI_RUNTIME_SERVICES *rt, EFI_GUID *vendor,
                          const CHAR16 *name, const CHAR16 *val)
{
    return rt->SetVariable((CHAR16 *)(uintptr_t)name, vendor, BLI_ATTRS,
                           (ulen(val) + 1u) * sizeof(CHAR16),
                           (void *)(uintptr_t)val);
}

static EFI_STATUS set_dec(EFI_RUNTIME_SERVICES *rt, EFI_GUID *vendor,
                          const CHAR16 *name, UINT64 v)
{
    CHAR16 buf[21];

    if (tsc_dec16(v, 0, buf, 21u) == 0)
        return EFI_BUFFER_TOO_SMALL;
    return set_str(rt, vendor, name, buf);
}

/* "<major>.<minor>" of a revision word, as "%u.%02u" of rev >> 16 and
 * rev & 0xffff: 2.70 for UEFI 2.7, 1.00 for OVMF's 0x10000. */
static UINTN put_rev(CHAR16 *out, UINTN at, UINTN cap, UINT32 rev)
{
    UINTN n;

    n = tsc_dec16(rev >> 16, 0, out + at, cap - at);
    if (n == 0)
        return 0;
    at += n;
    if (at + 1u >= cap)
        return 0;
    out[at++] = L'.';
    n = tsc_dec16(rev & 0xFFFFu, 2u, out + at, cap - at);
    if (n == 0)
        return 0;
    return at + n;
}

static void keep(EFI_STATUS *first, EFI_STATUS s)
{
    if (EFI_ERROR(s) && !EFI_ERROR(*first))
        *first = s;
}

EFI_STATUS bli_set_init(EFI_SYSTEM_TABLE *st, UINT64 usec,
                        const CHAR16 *info)
{
    EFI_GUID loader = BLI_LOADER_GUID;
    EFI_RUNTIME_SERVICES *rt;
    EFI_STATUS first = EFI_SUCCESS;
    CHAR16 buf[BLI_STR_MAX];
    UINTN at;

    if (st == (EFI_SYSTEM_TABLE *)0)
        return EFI_INVALID_PARAMETER;
    rt = st->RuntimeServices;
    if (rt == (EFI_RUNTIME_SERVICES *)0 || rt->SetVariable == (void *)0)
        return EFI_UNSUPPORTED;

    if (usec != 0)
        keep(&first, set_dec(rt, &loader, L"LoaderTimeInitUSec", usec));
    if (info != (const CHAR16 *)0)
        keep(&first, set_str(rt, &loader, L"LoaderInfo", info));

    at = st->FirmwareVendor != (CHAR16 *)0 ? ulen(st->FirmwareVendor)
                                            : BLI_VENDOR_MAX + 1u;
    if (at <= BLI_VENDOR_MAX) {
        UINTN i;

        for (i = 0; i < at; i++)
            buf[i] = st->FirmwareVendor[i];
        buf[at++] = L' ';
        at = put_rev(buf, at, BLI_STR_MAX, st->FirmwareRevision);
        if (at != 0)
            keep(&first, set_str(rt, &loader, L"LoaderFirmwareInfo", buf));
    }

    buf[0] = L'U';
    buf[1] = L'E';
    buf[2] = L'F';
    buf[3] = L'I';
    buf[4] = L' ';
    if (put_rev(buf, 5u, BLI_STR_MAX, st->Hdr.Revision) != 0)
        keep(&first, set_str(rt, &loader, L"LoaderFirmwareType", buf));
    return first;
}

static void put_a(char *out, UINTN cap, UINTN *at, const char *s)
{
    while (*s != '\0' && *at + 1u < cap)
        out[(*at)++] = *s++;
}

static void put_u(char *out, UINTN cap, UINTN *at, UINT64 v)
{
    CHAR16 dec[21];
    UINTN n = tsc_dec16(v, 0, dec, 21u);
    UINTN i;

    for (i = 0; i < n && *at + 1u < cap; i++)
        out[(*at)++] = (char)dec[i];
}

UINTN bli_kdf_info(const bli_kdf *k, char *out, UINTN cap)
{
    UINTN at = 0;

    if (out == (char *)0 || cap == 0)
        return 0;
    out[0] = '\0';
    if (k == (const bli_kdf *)0 || cap < BLI_KDF_INFO_MAX)
        return 0;
    if (k->kind == BLI_KDF_ARGON2ID) {
        put_a(out, cap, &at, "argon2id ");
        put_a(out, cap, &at, k->level != (const char *)0 ? k->level : "?");
        put_a(out, cap, &at, " ");
    } else if (k->kind == BLI_KDF_PBKDF2) {
        put_a(out, cap, &at, "pbkdf2 ");
    } else {
        put_a(out, cap, &at, "none ");
    }
    put_u(out, cap, &at, k->procs);
    put_a(out, cap, &at, " processors ");
    if (k->kind == BLI_KDF_ARGON2ID) {
        put_u(out, cap, &at, k->lanes);
        put_a(out, cap, &at, " lanes ");
        put_u(out, cap, &at, k->memory_kib);
        put_a(out, cap, &at, " KiB t=");
        put_u(out, cap, &at, k->passes);
        put_a(out, cap, &at, " ");
    } else if (k->kind == BLI_KDF_PBKDF2) {
        put_u(out, cap, &at, k->iterations);
        put_a(out, cap, &at, " iterations ");
    }
    put_u(out, cap, &at, k->usec / 1000u);
    put_a(out, cap, &at, " ms");
    if (k->mhz != 0) {
        put_a(out, cap, &at, " ");
        put_u(out, cap, &at, k->mhz);
        put_a(out, cap, &at, " MHz");
    }
    out[at] = '\0';
    return at;
}

static EFI_STATUS set_kdf(EFI_RUNTIME_SERVICES *rt, EFI_GUID *falcon,
                          const bli_kdf *kdf)
{
    EFI_STATUS first = EFI_SUCCESS;
    char line[BLI_KDF_INFO_MAX];
    CHAR16 val[BLI_KDF_INFO_MAX];
    UINTN n, i;

    keep(&first, set_dec(rt, falcon, BLI_KDF_NAME, kdf->usec));
    n = bli_kdf_info(kdf, line, sizeof line);
    for (i = 0; i <= n; i++)
        val[i] = (CHAR16)(unsigned char)line[i];
    keep(&first, set_str(rt, falcon, BLI_KDF_INFO_NAME, val));
    return first;
}

EFI_STATUS bli_set_exec(EFI_SYSTEM_TABLE *st, UINT64 usec, UINT64 wait_usec,
                        const bli_kdf *kdf)
{
    EFI_GUID loader = BLI_LOADER_GUID;
    EFI_GUID falcon = FALCON_VENDOR_GUID;
    EFI_RUNTIME_SERVICES *rt;
    EFI_STATUS first = EFI_SUCCESS;

    if (st == (EFI_SYSTEM_TABLE *)0)
        return EFI_INVALID_PARAMETER;
    rt = st->RuntimeServices;
    if (rt == (EFI_RUNTIME_SERVICES *)0 || rt->SetVariable == (void *)0)
        return EFI_UNSUPPORTED;
    if (usec == 0)
        return EFI_SUCCESS;
    keep(&first, set_dec(rt, &loader, L"LoaderTimeExecUSec", usec));
    keep(&first, set_dec(rt, &falcon, BLI_WAIT_NAME, wait_usec));
    if (kdf != (const bli_kdf *)0)
        keep(&first, set_kdf(rt, &falcon, kdf));
    return first;
}

void bli_clear_exec(EFI_SYSTEM_TABLE *st)
{
    EFI_GUID loader = BLI_LOADER_GUID;
    EFI_GUID falcon = FALCON_VENDOR_GUID;
    EFI_RUNTIME_SERVICES *rt;

    if (st == (EFI_SYSTEM_TABLE *)0)
        return;
    rt = st->RuntimeServices;
    if (rt == (EFI_RUNTIME_SERVICES *)0 || rt->SetVariable == (void *)0)
        return;
    (void)rt->SetVariable((CHAR16 *)(uintptr_t)L"LoaderTimeExecUSec", &loader,
                          BLI_ATTRS, 0, (void *)0);
    (void)rt->SetVariable((CHAR16 *)(uintptr_t)BLI_WAIT_NAME, &falcon,
                          BLI_ATTRS, 0, (void *)0);
    (void)rt->SetVariable((CHAR16 *)(uintptr_t)BLI_KDF_NAME, &falcon,
                          BLI_ATTRS, 0, (void *)0);
    (void)rt->SetVariable((CHAR16 *)(uintptr_t)BLI_KDF_INFO_NAME, &falcon,
                          BLI_ATTRS, 0, (void *)0);
}
