/* falcon-boot: the blessed-boot record cache (docs/DESIGN.md Section 8.1).
 *
 * The half of the blessed-boot path that has no firmware in it: the records
 * read off p2's manifest, which volume each came from, and the Section 8.1 rule
 * that turns a menu entry's path and current size into "not blessed", "blessed"
 * or "blessed but stale". falcon-efi/src/bless/bless.c is the other half and
 * does the reading, the launching and the console work.
 *
 * Split for the same reason falcon-core/src/fs/exfat.c and
 * falcon-core/src/manifest/manifest.c are EFI-free: this is where the rule that
 * decides what a user sees in the menu lives, and a rule that can only be
 * exercised by booting a machine is a rule nobody checks. Everything here runs
 * unchanged in a host process against a string literal.
 *
 * Security posture, restated because it is the thing most likely to be
 * "hardened" by mistake. The manifest is plain text on writable, unverified
 * exFAT: anyone with the stick edits it freely, and nothing in it is trusted.
 * It selects which signed UKI a menu entry launches; it does not decide
 * whether that UKI may run, which is shim's answer at load time and the
 * platform's to enforce (Sections 7.2, 7.6). So the failure this code must
 * never have is not "a forged record got in" (a forged record can only name
 * an image the platform will judge on its own merits) but "bad input turned
 * into bad behaviour": a path that matches the wrong file, an out-of-bounds
 * read, or an error the loader has to handle at menu-draw time. Hence: the
 * lookup is total, a manifest that will not parse yields zero records, and
 * an absent one is indistinguishable from an empty one.
 *
 * The deliberate non-check is the hash. Section 8.1 has the loader compare
 * size only, because hashing a multi-gigabyte ISO to draw a menu is not a
 * boot loader's job and the boot's security does not rest on the answer.
 */

#ifndef FALCON_BLESS_CORE_H
#define FALCON_BLESS_CORE_H

#include <stdint.h>
#include <stddef.h>

#include <media/family.h>
#include <iso/overlay.h>
#include <manifest/manifest.h>

/* How many blessed ISOs one stick may offer.
 *
 * A bound, not a format limit: the cache is static storage in the loader
 * (~16 KB at this cap) because there is no allocator here that a menu draw
 * may depend on, and a manifest is a file an attacker can make arbitrarily
 * long. Thirty-two blessed ISOs is far past what fits on a stick once each
 * one costs a signed UKI wrap of its own, and the overflow is reported
 * rather than hidden (see bless_cache::dropped). */
#define BLESS_MAX_RECORDS 32u

/* Largest manifest the loader will read. Each record renders to roughly 200
 * bytes, so 64 KiB is two hundred times the cap above: a file larger than
 * this is not a manifest anyone wrote, and reading it would only produce
 * records there is no room for. The reader stops at the cap rather than
 * refusing, so a padded manifest still boots the ISOs it names first. */
#define BLESS_MANIFEST_MAX ((uint64_t)64u << 10)

/* What a menu entry is. Same three answers as manifest_bless, restated in
 * this module's vocabulary because this is the enum the menu and the boot
 * dispatch branch on, and because Section 8.1's rule is that STALE boots
 * exactly as NONE does: the label differs, the boot path does not. */
typedef enum {
    BLESS_NONE  = 0,  /* no record names this file */
    BLESS_FRESH = 1,  /* record found and the size still agrees */
    BLESS_STALE = 2   /* record found, the file changed since it was blessed */
} bless_state;

/* The records from every ISO-role volume, each tagged with the volume it was
 * read from.
 *
 * One pool rather than one per volume: the cap is a budget for the loader,
 * not for a partition, and a second stick plugged in beside the first should
 * not double the loader's static footprint. The tag is what keeps two sticks
 * that both bless "Linux/arch.iso" from answering for each other; it is an
 * opaque pointer here because this module has no business knowing what an
 * fs_vol is. */
typedef struct {
    manifest_record rec[BLESS_MAX_RECORDS];
    const void     *src[BLESS_MAX_RECORDS]; /* volume tag, parallel to rec */
    size_t          n;
    /* Valid records the manifests held past the cap. Reported so the loader
     * can say "three blessed ISOs are not shown" rather than leaving a user
     * to wonder why an entry they blessed lost its label. */
    size_t          dropped;
} bless_cache;

/* A match, with enough to launch it: the record names the UKI, the tag says
 * which volume to open it on. Valid only while the cache is unchanged. */
typedef struct {
    const manifest_record *rec;
    const void            *src;
    bless_state            state;
} bless_hit;

void bless_cache_init(bless_cache *c);

/* Parses one volume's manifest and appends what fits, tagging each record
 * with src. Returns the number of records appended.
 *
 * Never fails and never reports an error: a buffer that parses to nothing
 * (absent file, truncated read, a JPEG someone renamed) appends nothing,
 * which is Section 8.1's "corrupt or absent manifest degrades to nothing is
 * blessed". Records the buffer held beyond the remaining room are counted in
 * c->dropped. Passing NULL/0 is the "there was no manifest" call and behaves
 * exactly like an empty one. */
size_t bless_cache_add(bless_cache *c, const void *buf, size_t len,
                       const void *src);

/* The Section 8.1 rule for one menu entry: is the file at this UCS-2 path on
 * this volume blessed, and is the blessing still fresh?
 *
 * path is the entry's path relative to the volume root, as the scan built it
 * ('/'-separated; a backslash-separated one compares equal, which is
 * manifest_path_eq's contract). A path that is not printable ASCII cannot
 * match any record, because manifest_parse drops records that contain such a
 * byte, so it answers NONE without searching.
 *
 * *out, when non-NULL, is filled in on every call, including BLESS_NONE
 * (rec and src NULL), so a caller cannot read a stale hit by forgetting to
 * check the return. */
bless_state bless_cache_lookup(const bless_cache *c, const void *src,
                               const uint16_t *path, uint64_t cur_size,
                               bless_hit *out);

/* ---- what a hit means (DESIGN Section 8.1) ---- */

/* Which launch path a matched entry takes.
 *
 * Stated once, here, rather than re-derived from the state at each call
 * site, because getting it wrong is silent: chainloading media the user
 * blessed precisely to avoid chainloading. STALE is PLAIN, Section 8.1's
 * rule: the wrap was built from bytes the file no longer has. A record of
 * the retired prepared kind never gets this far: the manifest parser skips
 * it, so its ISO has no hit at all. */
typedef enum {
    BLESS_ACT_PLAIN   = 0,  /* boot the medium as it sits on p2 */
    BLESS_ACT_UKI     = 1   /* fresh blessed record: launch the signed wrap */
} bless_action;

bless_action bless_hit_action(const bless_hit *h);

/* The label suffix for a hit, or NULL when the entry has earned none.
 *
 * Strings rather than drawing: the menu is presentation and this module is
 * not it, so the caller composes. The wording follows the same rule the ISO
 * survey labels do (DESIGN Section 9): it reports what was found on the
 * stick, never a verdict about what will be allowed to run. "blessed" means
 * a manifest record names a signed wrap for this ISO, not that anything
 * verified it; shim decides that at load time. */
const uint16_t *bless_suffix(const bless_hit *h);

/* The next-boot marker's value rules are falcon-core's (media/nextboot.h),
 * shared with falcon-bios. */

#endif /* FALCON_BLESS_CORE_H */
