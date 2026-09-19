/* falcon-boot: Windows install media, patched at boot (docs/DESIGN.md
 * Section 6.3, moved into the loader by Section 6.4; docs/notes/
 * coverage-roadmap.md M6).
 *
 * A stock Windows ISO boots through the virtual CD to WinPE and then Setup
 * cannot find its install source, because the ISO stopped existing at
 * ExitBootServices. The fix of Section 6.3 is falcon-winjump in place of
 * WinPE's winpeshl.exe, attaching the ISO file from Windows once it is up.
 * Here that patch is made as the ISO is served, with nothing prepared in
 * advance and nothing written anywhere: boot.wim is read out of the ISO's UDF
 * tree, the patch is computed in memory (falcon-core/src/wim/wim.h), and the
 * virtual CD answers with a longer boot.wim through the override layer
 * (falcon-efi/src/iso/overlay.h). Its UDF File Entry, the Partition
 * Descriptors and a second anchor are rewritten to match
 * (falcon-core/src/fs/udf.h).
 *
 * Nothing the platform verifies is touched: bootmgr, winload and the kernel
 * stay Microsoft-signed and are served byte for byte, which is why a Windows
 * ISO boots this way with Secure Boot on.
 */

#ifndef FALCON_WINPATCH_H
#define FALCON_WINPATCH_H

#include <efi/uefi.h>
#include <fs/fs.h>

typedef struct {
    const CHAR16 *stage;
    EFI_STATUS    status;
    /* Set when the media is not one this path can patch (no boot.wim, not
     * LZX, no winpeshl.exe), before anything was served: the caller's cue
     * to offer the appliance's preparation instead. */
    UINT8         unpatchable;
    const CHAR16 *advice;
} win_result;

/* Boots the ISO iso on the mounted p2 volume, patched. iso_rel is its path
 * relative to p2's root, which falcon-winjump uses to find it again from
 * Windows. Returns as iso_boot_overlaid does. */
EFI_STATUS win_patched_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                            fs_vol *p2, const fs_dirent *iso,
                            const char *iso_rel, win_result *result);

#endif /* FALCON_WINPATCH_H */
