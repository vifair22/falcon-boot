/* falcon-boot: direct kernel boot of an ISO on p2 (docs/DESIGN.md Section
 * 6.4; docs/notes/coverage-roadmap.md W2 D1).
 *
 * The ISO's own boot configuration names a kernel, initrds and a cmdline for
 * booting from its CD (falcon-core/src/media/bootcfg.c). This path reads all
 * three out of the ISO file with falcon-core's ISO9660/UDF drivers, appends
 * falcon-hook to the initrd, publishes where the ISO sits on p2 as the
 * FalconIsoMap variable, and starts the kernel through the shim-verified load
 * path with the hooked cmdline (falcon-core/src/media/hookcmd.c). falcon-hook
 * then rebuilds the ISO as /dev/mapper/falcon-iso, under the ISO's own label,
 * and the distro's initramfs finds its CD where it expects one.
 *
 * The kernel is verified exactly as a chainloaded image is: through load_ops,
 * so under Secure Boot a kernel the platform does not trust is refused. That
 * is why the menu makes this the default only while nothing is enforcing,
 * and why blessing (DESIGN Section 8.1) is the Secure Boot route.
 *
 * The initrd reaches the kernel over LoadFile2 (efi/loadfile2.h) and the
 * cmdline as the image's LoadOptions, the two ways every EFI-stub kernel since
 * 5.8 takes them.
 */

#ifndef FALCON_DIRECT_H
#define FALCON_DIRECT_H

#include <efi/uefi.h>
#include <fs/fs.h>
#include <media/bootcfg.h>

typedef struct {
    const CHAR16 *stage;   /* what was being attempted */
    EFI_STATUS    status;  /* firmware status there, or the kernel's exit */
    /* Set when the ISO has no configuration entry this path can boot, before
     * anything was loaded: the caller's cue to chainload instead. */
    UINT8         no_entry;
    char          title[BOOTCFG_TITLE_MAX];   /* the entry, once picked */
    /* One line to add to the failure, or NULL. */
    const CHAR16 *advice;
} direct_result;

/* Boots iso, a file on the mounted p2 volume p2. p2_handle is p2's partition
 * handle, whose device path names the partition for the extent map, and
 * block_size its logical block size.
 *
 * Returns EFI_SUCCESS when the kernel was started and returned (result->status
 * is its exit status); any other value is the status that ended the attempt,
 * with result->stage naming where. Everything published for the attempt (the
 * variable, the LoadFile2 handle, the buffers) is gone again on return. */
/* How the kernel is started. KERNEL loads it through load_ops, which is
 * what boots it with Secure Boot off. ISO_SHIM loads the ISO's own shim
 * instead, which verifies the kernel against its vendor's certificate: on a
 * machine whose db trusts the Microsoft CA, a stock distro ISO boots with
 * Secure Boot on and no blessing (docs/notes/coverage-roadmap.md W2 D3a). */
typedef enum {
    DIRECT_VIA_KERNEL = 0,
    DIRECT_VIA_ISO_SHIM
} direct_via;

EFI_STATUS direct_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent, fs_vol *p2,
                       EFI_HANDLE p2_handle, UINT32 block_size,
                       const fs_dirent *iso, direct_via via,
                       direct_result *result);

/* Sets FalconIsoMap for iso without booting anything: for a blessed wrap,
 * whose initrd carries falcon-hook too (falcon-cli/src/sign/main.c). The
 * caller clears it with loader_isomap_clear if the image comes back.
 *
 * The map's bytes come back in *map and *len too (either may be NULL), for
 * the wrap's credential drop-in: a kernel without efivarfs never sees the
 * variable (bless_set_map). They stay valid until the next call. */
EFI_STATUS direct_map_set(EFI_SYSTEM_TABLE *st, fs_vol *p2,
                          EFI_HANDLE p2_handle, UINT32 block_size,
                          const fs_dirent *iso, const UINT8 **map,
                          UINTN *len);

/* Builds the map's bytes into *map and *len without setting FalconIsoMap: the
 * Windows path carries them in winjump's cfg instead of an EFI variable. The
 * bytes stay valid until the next call to either builder. */
EFI_STATUS direct_map_build(EFI_SYSTEM_TABLE *st, fs_vol *p2,
                            EFI_HANDLE p2_handle, UINT32 block_size,
                            const fs_dirent *iso, const UINT8 **map,
                            UINTN *len);

/* D3b (docs/notes/d3-spike.md): boots iso through its own loader chain
 * (its shim, its signed GRUB) with its grub.cfg's bytes replaced by one of
 * ours (falcon-core/src/media/grubcfg.h) and falcon-hook's archive served
 * past the ISO's end as one more initrd. For Secure Boot on, where the
 * kernel carries no .sbat and D3a cannot start it. result->no_entry as
 * direct_boot's: nothing this route can serve, chainload instead. */
EFI_STATUS direct_grub_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent,
                            fs_vol *p2, EFI_HANDLE p2_handle,
                            UINT32 block_size, const fs_dirent *iso,
                            direct_result *r);

#endif /* FALCON_DIRECT_H */
