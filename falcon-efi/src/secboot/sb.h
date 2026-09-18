/* falcon-boot: Secure Boot state, for UX only (docs/DESIGN.md Sections 7.3,
 * 7.4, 9).
 *
 * This module reads three EFI variables and says what they mean in words the
 * menu can print. That is its entire job. Nothing here decides what may boot:
 * enforcement belongs to shim and the firmware, never to this loader, and no
 * caller may use these values to gate a load. The invariant is worth stating
 * at the top of the header because the temptation to "just check secure_boot
 * before launching" is exactly the regression DESIGN Section 7.3 forbids.
 *
 * A variable that is absent is a defined state, not a failure: no MokSBStateRT
 * means shim validation is enabled, which is the common case and the one the
 * banner must render as normal rather than as an error.
 */

#ifndef FALCON_SB_H
#define FALCON_SB_H

#include <efi/uefi.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    SB_OK          = 0,
    SB_ERR_BAD_ARG = -1,
};

/* Firmware answers are three-valued: the variable said yes, the variable said
 * no, or we could not get a trustworthy answer (no runtime services, an
 * unexpected size, a device error). "Unknown" is never silently folded into
 * "no", because "Secure Boot is off" and "we do not know" are different
 * things to tell a user. */
typedef enum {
    SB_TRI_UNKNOWN = 0,
    SB_TRI_NO      = 1,
    SB_TRI_YES     = 2,
} sb_tri;

typedef struct {
    sb_tri secure_boot;                 /* EFI global SecureBoot */
    sb_tri setup_mode;                  /* EFI global SetupMode */
    sb_tri mok_sb_validation_disabled;  /* shim MokSBStateRT */
} sb_state;

/* The states worth distinguishing on screen. Derived from sb_state so that
 * the precedence rules live in one place instead of in every caller. */
typedef enum {
    SB_MODE_UNKNOWN = 0,
    SB_MODE_OFF,
    SB_MODE_ON,
    SB_MODE_ON_VALIDATION_DISABLED,
    SB_MODE_SETUP,
} sb_mode;

/* Fills out from the platform's variables. Returns SB_ERR_BAD_ARG only for a
 * NULL out; a NULL runtime-services pointer yields an all-unknown state,
 * which is a legitimate answer rather than an error. */
int sb_state_read(EFI_RUNTIME_SERVICES *rt, sb_state *out);

sb_mode sb_state_mode(const sb_state *s);

/* Short banner label, e.g. L"SB: on (shim validation DISABLED)". Never NULL,
 * and never wider than 33 columns, so it still fits beside the title in the
 * spec's guaranteed 80x25 mode. */
const CHAR16 *sb_mode_label(sb_mode m);

/* Nonzero when the mode must not slip past the user: Secure Boot looks on,
 * yet shim has been told to stop validating, so this machine runs unsigned
 * EFI code (DESIGN Section 7.4). The caller renders it accordingly; this
 * function grants no permission and changes nothing. */
int sb_mode_needs_warning(sb_mode m);

#endif /* FALCON_SB_H */
