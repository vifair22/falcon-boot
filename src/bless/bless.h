/* falcon-boot: blessed boot (docs/DESIGN.md Section 8.1).
 *
 * The path a menu entry takes when the ISO it names has been blessed: instead
 * of chainloading the ISO's own bootloader off a virtual CD (src/iso), the
 * loader launches the signed UKI wrap that `falcon-boot-sign` built for it,
 * whose embedded cmdline points the distro's initramfs back at the ISO file
 * on p2. That wrap is kernel + cmdline + initrd inside one signed PE, which
 * is what lets the ISO boot with Secure Boot validation left on: everything
 * the boot executes is covered by a signature the machine has enrolled.
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

/* Where a bless lives on p2, matching `falcon-boot-sign`'s BLESSED_DIR and
 * the reserved directory of DESIGN Section 6.2. Two components, walked one at
 * a time because the filesystem registry hands out directory iterators rather
 * than a path resolver. */
#define BLESS_DIR_L1 "falcon"
#define BLESS_DIR_L2 "blessed"
#define BLESS_MANIFEST_NAME "manifest"

/* Where a prepared medium's sidecars live: p2:/falcon/prepared/<sha256>/,
 * three components under the same reserved directory (DESIGN Section 6.3.1).
 * The last one is the record's own sha256=, which is what keeps the link
 * between an ISO and its artifacts intrinsic rather than by file name. */
#define BLESS_DIR_PREPARED "prepared"

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

/* Launches the blessed UKI the hit names.
 *
 * Returns EFI_SUCCESS when the image ran and returned (result->status is its
 * exit status, which for a real sd-stub wrap means it failed to boot a
 * kernel); any other value is the firmware status that ended the attempt,
 * with result->stage naming where and result->advice carrying the one line
 * worth acting on. EFI_SECURITY_VIOLATION is the platform refusing the image
 * and is reported as such: the caller must NOT respond by booting the ISO the
 * unblessed way, because that is the path the user blessed the ISO to avoid
 * (see the dispatch note in src/efi/main.c).
 *
 * Everything allocated is released before returning, so a refused entry can
 * be retried and the menu comes back intact. */
EFI_STATUS bless_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                      const bless_hit *hit, bless_result *result);

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

/* Boots a PREPARED medium: the ISO itself, served with the byte map its
 * record asks for (DESIGN Sections 6.3 and 6.3.1).
 *
 * Nothing is signed and nothing is verified differently here. The overlay
 * changes which bytes the virtual medium serves; every image the platform
 * checks on a Windows boot stays Microsoft-signed and untouched, and the
 * payload is named to LoadImage by device path exactly as an unprepared one
 * is (see iso_boot_overlaid).
 *
 * Fails closed and says why. A splice file that is absent, a record with no
 * byte map, one asking for more ranges than a medium may carry, and a sidecar
 * too short for the range it backs are all refusals with an actionable line in
 * result->advice - never a quiet fallback to booting the ISO unprepared, which
 * is the one outcome the user cannot diagnose: Windows Setup would come up and
 * then fail to find its install source, exactly as it does with no preparation
 * at all, with nothing on screen to say the preparation was the broken part.
 *
 * vol and file are the medium's own volume and directory entry, as the scan
 * found them; hit->src named the volume the record was read from, which is the
 * same volume by construction (bless_cache_lookup matches on it).
 *
 * Returns EFI_SUCCESS when the payload ran and returned, with result->status
 * its exit status; any other value is the status that ended the attempt. */
EFI_STATUS bless_prepared_boot(EFI_SYSTEM_TABLE *st, EFI_HANDLE parent_image,
                               const bless_hit *hit, fs_vol *vol,
                               const fs_dirent *file, iso_media_kind kind,
                               bless_result *result);

#endif /* FALCON_BLESS_H */
