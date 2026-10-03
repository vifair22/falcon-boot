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

EFI_STATUS bli_set_exec(EFI_SYSTEM_TABLE *st, UINT64 usec, UINT64 wait_usec)
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
    return first;
}
