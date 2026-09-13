/* Secure Boot state for UX only. See sb.h for the invariant this module must
 * not be made to violate.
 *
 * Variable sources:
 *   SecureBoot, SetupMode   EFI global variable GUID, one UINT8 each
 *                           (UEFI spec 3.3, Table "Global Variables")
 *   MokSBStateRT            shim lock GUID, one UINT8, set by shim only when
 *                           the user ran mokutil --disable-validation and
 *                           confirmed at MokManager. Absent is the normal,
 *                           validating case.
 */

#include <sb/sb.h>

/* 8be4df61-93ca-11d2-aa0d-00e098032b8c */
#define EFI_GLOBAL_VARIABLE_GUID \
    { 0x8be4df61, 0x93ca, 0x11d2, \
      { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } }

/* 605dab50-e046-4300-abb6-3dd810dd8b23 */
#define SHIM_LOCK_GUID \
    { 0x605dab50, 0xe046, 0x4300, \
      { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 } }

/* Reads a one-byte variable. A variable the firmware reports as not present
 * is a defined answer (all three of ours mean "no" by their absence), so it
 * maps to SB_TRI_NO; only a variable that exists and cannot be read as one
 * sane byte leaves the answer unknown. */
static sb_tri read_bool_var(EFI_RUNTIME_SERVICES *rt, const CHAR16 *name,
                            const EFI_GUID *guid)
{
    EFI_GUID vendor = *guid;
    UINT8 value = 0;
    UINTN size = sizeof(value);
    EFI_STATUS status;

    /* GetVariable takes non-const pointers because the spec declares it so,
     * not because the firmware writes through them; the uintptr_t round trip
     * is what keeps -Wcast-qual off a wart that is the spec's, not ours. */
    status = rt->GetVariable((CHAR16 *)(uintptr_t)name, &vendor,
                             (UINT32 *)0, &size, &value);
    if (status == EFI_NOT_FOUND)
        return SB_TRI_NO;
    if (EFI_ERROR(status) || size != sizeof(value))
        return SB_TRI_UNKNOWN;
    return value ? SB_TRI_YES : SB_TRI_NO;
}

int sb_state_read(EFI_RUNTIME_SERVICES *rt, sb_state *out)
{
    EFI_GUID global = EFI_GLOBAL_VARIABLE_GUID;
    EFI_GUID shim = SHIM_LOCK_GUID;

    if (out == (void *)0)
        return SB_ERR_BAD_ARG;

    out->secure_boot = SB_TRI_UNKNOWN;
    out->setup_mode = SB_TRI_UNKNOWN;
    out->mok_sb_validation_disabled = SB_TRI_UNKNOWN;

    if (rt == (void *)0 || rt->GetVariable == (void *)0)
        return SB_OK;

    out->secure_boot = read_bool_var(rt, L"SecureBoot", &global);
    out->setup_mode = read_bool_var(rt, L"SetupMode", &global);

    /* Shim mirrors the boot-services-only MokSBState into MokSBStateRT so
     * that it survives into runtime; the variable exists only on a machine
     * where the user ran mokutil --disable-validation and confirmed it at
     * MokManager, so absence is the ordinary validating case. */
    out->mok_sb_validation_disabled = read_bool_var(rt, L"MokSBStateRT",
                                                    &shim);
    return SB_OK;
}

sb_mode sb_state_mode(const sb_state *s)
{
    if (s == (void *)0)
        return SB_MODE_UNKNOWN;

    if (s->secure_boot == SB_TRI_YES) {
        /* Setup mode and an asserted SecureBoot cannot coexist on a sane
         * platform; if they do, enforcement is what matters, so it wins. */
        if (s->mok_sb_validation_disabled == SB_TRI_YES)
            return SB_MODE_ON_VALIDATION_DISABLED;
        return SB_MODE_ON;
    }
    /* Setup mode ranks above plain "off" because it says why nothing is being
     * enforced: the platform has no PK, so db is not in force yet. */
    if (s->setup_mode == SB_TRI_YES)
        return SB_MODE_SETUP;
    if (s->secure_boot == SB_TRI_NO)
        return SB_MODE_OFF;
    return SB_MODE_UNKNOWN;
}

const CHAR16 *sb_mode_label(sb_mode m)
{
    switch (m) {
    case SB_MODE_OFF:
        return L"SB: off";
    case SB_MODE_ON:
        return L"SB: on";
    case SB_MODE_ON_VALIDATION_DISABLED:
        return L"SB: on (shim validation disabled)";
    case SB_MODE_SETUP:
        return L"SB: setup mode";
    case SB_MODE_UNKNOWN:
    default:
        return L"SB: unknown";
    }
}

int sb_mode_needs_warning(sb_mode m)
{
    return m == SB_MODE_ON_VALIDATION_DISABLED;
}
