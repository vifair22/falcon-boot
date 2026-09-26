/* falcon-boot: blessed boot (docs/DESIGN.md Section 8.1).
 *
 * The path a menu entry takes when the ISO it names has been blessed: instead
 * of chainloading the ISO's own bootloader off a virtual CD
 * (falcon-efi/src/iso), the loader launches the signed UKI wrap that
 * `falcon-boot-sign` built for it, whose embedded cmdline points the distro's
 * initramfs back at the ISO file on p2. That wrap is kernel + cmdline + initrd
 * inside one signed PE, which is what lets the ISO boot with Secure Boot
 * validation left on: everything the boot executes is covered by a signature
 * the machine has enrolled.
 *
 * What this module is NOT. It does not verify the UKI and it holds no key:
 * the image is handed to the same shim-verified load path everything else
 * goes through (DESIGN Section 7.6), which enforces when Secure Boot is on
 * and permits when it is off, and a refusal here is the platform's answer
 * reported verbatim. There is no override, no fallback that relaxes
 * anything, and no path by which a manifest edit changes what may run: the
 * manifest chooses which image is offered to the platform, never whether the
 * platform accepts it (see blesscore.h for the full posture).
 *
 * Two halves, split by whether firmware is involved. blesscore.{h,c} holds
 * the record cache and the Section 8.1 freshness rule and is host-tested;
 * this header is the firmware half: reading the manifest and the UKI off the
 * volume, publishing the image on the synthetic filesystem (DESIGN Section
 * 5.1, reused here without a credential), and the launch.
 */

#ifndef FALCON_BLESS_H
#define FALCON_BLESS_H

#include <efi/uefi.h>

#include <bless/blesscore.h>
#include <fs/fs.h>
#include <iso/iso.h>
#include <trust/trust.h>

/* Where a bless lives on p2, matching `falcon-boot-sign`'s BLESSED_DIR and
 * the reserved directory of DESIGN Section 6.2. Two components, walked one at
 * a time because the filesystem registry hands out directory iterators rather
 * than a path resolver. */
#define BLESS_DIR_L1 "falcon"
#define BLESS_DIR_L2 "blessed"
#define BLESS_MANIFEST_NAME "manifest"

/* A sanity bound on the UKI, not a format limit. Section 8.1 puts a blessed
 * wrap at 100-250 MB because it embeds the ISO's initramfs, so this is a few
 * times the largest plausible one; a "UKI" past it is a corrupt directory
 * entry, and finding that out before allocating half a gigabyte is the
 * difference between a failed entry and a hung menu. */
#define BLESS_UKI_MAX ((UINT64)768u << 20)

/* Why a blessed boot ended, in words fit for the menu's status line. Same
 * shape as pos_result and iso_boot_result, because main.c reports all three
 * the same way. */
typedef struct {
    const CHAR16 *stage;   /* what was being attempted */
    EFI_STATUS    status;  /* firmware status there, or the UKI's exit status */
    /* One actionable line, or NULL. Carried rather than printed here so the
     * wording for a refusal lives next to the code that knows which refusal
     * it was. */
    const CHAR16 *advice;
} bless_result;

/* Forgets every record. Call before a rescan; bless_load appends. */
void bless_reset(void);

/* Reads /falcon/blessed/manifest off one ISO-role volume and adds what it
 * holds to the cache, tagged with that volume. Returns the number of records
 * added.
 *
 * Never fails: a volume with no /falcon, no manifest, an unreadable one, or
 * one full of anything but manifest records adds nothing and is not an error
 * (DESIGN Section 8.1). The volume pointer is kept, so it must stay mounted
 * for as long as its entries can be booted, which on this loader means for
 * the life of the process.
 *
 * st is needed for the transient read buffer (BLESS_MANIFEST_MAX from the
 * pool, released before returning), so the cache costs nothing but its own
 * static storage between calls. */
UINTN bless_load(EFI_SYSTEM_TABLE *st, fs_vol *vol);

/* The Section 8.1 rule for one menu entry; see bless_cache_lookup. path is
 * the entry's path relative to the volume root and cur_size the size the
 * directory listing reported for it just now. */
bless_state bless_lookup(const fs_vol *vol, const CHAR16 *path,
                         UINT64 cur_size, bless_hit *out);

/* Records that were parsed but had nowhere to go (BLESS_MAX_RECORDS). */
UINTN bless_dropped(void);

/* The boot prediction for the wrap a fresh record names (DESIGN Section 7.8):
 * its certificate table and .sbat, read off p2 without reading the image. The
 * wrap is ~250 MB, well past TRUST_HASH_MAX, so no hash is taken and the
 * verdict is the signer's. TRUST_NO_VERDICT on anything unusual. A label: the
 * launch below neither takes nor reads it. */
void bless_uki_trust(const bless_hit *hit, const trust_stores *stores,
                     trust_result *out);

/* Launches the blessed UKI the hit names.
 *
 * Returns EFI_SUCCESS when the image ran and returned (result->status is its
 * exit status, which for a real sd-stub wrap means it failed to boot a
 * kernel); any other value is the firmware status that ended the attempt,
 * with result->stage naming where and result->advice carrying the one line
 * worth acting on. EFI_SECURITY_VIOLATION is the platform refusing the image
 * and is reported as such: the caller must NOT respond by booting the ISO the
 * unblessed way, because that is the path the user blessed the ISO to avoid
 * (see the dispatch note in apps/stick/efi_main.c).
 *
 * Everything allocated is released before returning, so a refused entry can
 * be retried and the menu comes back intact. */
EFI_STATUS bless_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                      const bless_hit *hit, bless_result *result);

/* The same launch, with the image named to the platform as load_path instead
 * of being published on the synthetic filesystem.
 *
 * One caller: a blessed BSD medium (falcon-efi/src/bsd). What is signed there
 * is a copy of the medium's own loader, and that loader works out the device
 * it came from and reads its kernel, its configuration and its memory disk
 * from it. Published on a filesystem with no Block I/O behind it, it finds
 * nothing and stops at "Failed to find bootable partition"; named as the
 * medium's own \EFI\BOOT\BOOTX64.EFI, it finds the medium, which is where
 * those files are.
 *
 * The bytes handed to LoadImage are still the signed copy off p2, and those
 * are what the platform verifies. The path only says which device the
 * started image should treat as its own.
 *
 * load_path NULL is exactly bless_boot. */
EFI_STATUS bless_boot_as(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                         const bless_hit *hit,
                         EFI_DEVICE_PATH_PROTOCOL *load_path,
                         bless_result *result);

/* The ISO's extent map (falcon-core/src/media/extmap.h) for the next
 * bless_boot to publish beside the wrap as <uki>.extra.d/falcon-map.cred,
 * which sd-stub packs into the initrd at /.extra/credentials/ where
 * falcon-hook looks for it. NULL clears it. Not secret: it says where a
 * file sits on p2. The bytes must outlive the boot. */
void bless_set_map(const UINT8 *map, UINTN len);

/* Launches a BARE EFI PAYLOAD: a .efi file on p2 that boots as itself
 * (DESIGN Section 7.2).
 *
 * Not a blessing, and it consults no manifest: there is no second artifact to
 * point at, because the file the scan found IS the image the platform
 * verifies. It lives here because the mechanism is identical to the blessed
 * wrap's and must not be reinvented - the payload is read off exFAT, published
 * on the synthetic filesystem of DESIGN Section 5.1 so it has a device path at
 * all, and handed to the shim-verified load path of Section 7.6, which
 * enforces when Secure Boot is on and permits when it is off.
 *
 * Nothing is verified here and nothing is relaxed. A payload the platform
 * refuses comes back EFI_SECURITY_VIOLATION with the two doors named, one of
 * which is signing the file itself, since that is what the platform is
 * looking at.
 *
 * vol and file are the payload's volume and directory entry as the scan found
 * them. Returns EFI_SUCCESS when the payload ran and returned, with
 * result->status its exit status; any other value is the status that ended the
 * attempt, result->stage naming where. */
EFI_STATUS bless_payload_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                              fs_vol *vol, const fs_dirent *file,
                              bless_result *result);

#endif /* FALCON_BLESS_H */
