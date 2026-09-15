/* falcon-boot: the blessed-boot record cache (docs/DESIGN.md Section 8.1).
 *
 * The half of the blessed-boot path that has no firmware in it: the records
 * read off p2's manifest, which volume each came from, and the Section 8.1
 * rule that turns a menu entry's path and current size into "not blessed",
 * "blessed" or "blessed but stale". src/bless/bless.c is the other half and
 * does the reading, the launching and the console work.
 *
 * Split for the same reason src/exfat and src/manifest are EFI-free: this is
 * where the rule that decides what a user sees in the menu lives, and a rule
 * that can only be exercised by booting a machine is a rule nobody checks.
 * Everything here runs unchanged in a host process against a string literal.
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
 * That rule covers BOTH kinds of record: a prepared one carries a sha256=
 * too, and it is read only as the NAME of the sidecar folder (Section 6.3.1),
 * never recomputed over the ISO.
 */

#ifndef FALCON_BLESS_CORE_H
#define FALCON_BLESS_CORE_H

#include <stdint.h>
#include <stddef.h>

#include <family/family.h>
#include <iso/overlay.h>
#include <manifest/manifest.h>

/* Status codes for the calls below that can fail, repo convention: OK zero,
 * errors negative. The lookups do not appear here; they are total. */
enum {
    BLESS_OK           =  0,
    BLESS_ERR_BAD_ARG  = -1,
    BLESS_ERR_KIND     = -2,  /* asked of a record of the other kind */
    BLESS_ERR_NOSPLICE = -3,  /* a prepared record with no byte map at all */
    BLESS_ERR_SPLICES  = -4,  /* more splices than one medium may carry */
    BLESS_ERR_VALUE    = -5   /* the next-boot marker is not a usable path */
};

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

/* ---- what a hit means (DESIGN Sections 8.1 and 6.3.1) ---- */

/* Which launch path a matched entry takes.
 *
 * Stated once, here, rather than re-derived from state and kind at each call
 * site, because the two ways of getting it wrong are both silent: chainloading
 * media the user blessed precisely to avoid chainloading, or serving a Windows
 * ISO without the byte map that makes Setup able to find its install source.
 *
 * STALE is PLAIN for both kinds, which is Section 8.1's rule extended to the
 * second kind for the same reason it holds for the first: the artifacts were
 * computed from bytes the file no longer has. */
typedef enum {
    BLESS_ACT_PLAIN   = 0,  /* boot the medium as it sits on p2 */
    BLESS_ACT_UKI     = 1,  /* fresh blessed record: launch the signed wrap */
    BLESS_ACT_OVERLAY = 2   /* fresh prepared record: serve it with its map */
} bless_action;

bless_action bless_hit_action(const bless_hit *h);

/* Where a Windows entry stands with respect to preparation (Section 6.3.1).
 *
 * `fam` is what the ISO survey found inside the medium, or FAMILY_UNKNOWN when
 * the entry was never probed. An unprobed entry with a fresh record is still
 * READY: the record says what it is, and the survey is only needed to notice
 * media that has NO record, which is the one answer the record cannot give. */
typedef enum {
    BLESS_PREP_NA = 0,   /* nothing to prepare, or nothing known to prepare */
    BLESS_PREP_READY,    /* a fresh prepared record stands behind it */
    BLESS_PREP_STALE,    /* a prepared record, but the ISO changed since */
    BLESS_PREP_ABSENT    /* Windows media and no prepared record at all */
} bless_prep;

bless_prep bless_prep_state(const bless_hit *h, family_id fam);

/* The label suffix for a hit, or NULL when the entry has earned none.
 *
 * Strings rather than drawing: the menu is presentation and this module is
 * not it, so the caller composes. The wording follows the same rule the ISO
 * survey labels do (DESIGN Section 9): it reports what was found on the
 * stick, never a verdict about what will be allowed to run. "blessed" means
 * a manifest record names a signed wrap for this ISO, not that anything
 * verified it; shim decides that at load time. "prepared" means a byte map
 * for it is staged on the stick, and nothing about Windows media is verified
 * by anyone here in the first place (Section 6.3: every component the platform
 * checks stays Microsoft-signed and untouched). */
const uint16_t *bless_suffix(const bless_hit *h, family_id fam);

/* ---- the byte map a prepared record asks for (DESIGN Section 6.3) ---- */

/* Turns a prepared record's splice list into an override set.
 *
 * One entry per splice, in the record's own order: the whole sidecar replaces
 * `len` bytes at `iso_off`, so src_off is zero. Every src is left NULL,
 * because opening files is the caller's side of the line and this module has
 * no filesystem.
 *
 * What is NOT checked here: ordering, overlap, range against the medium, and
 * whether each sidecar actually holds the bytes it promises. That is
 * iso_overlay_check's, which the boot path runs as a fail-closed gate with the
 * files open; a second implementation of the same rule could only be a second
 * answer to it. What IS checked is what cannot be represented at all: a
 * record of the wrong kind, a record with no splices (a preparation that did
 * nothing, which would serve the ISO unaltered under a label promising
 * otherwise), and more splices than one medium may carry. */
int bless_prepared_overlay(const manifest_record *r, iso_overlay *out);

/* ---- the next-boot marker (DESIGN Section 6.3.1) ---- */

/* Longest marker value worth looking at: the path of an ISO on p2, so the
 * manifest's own path cap is the bound, terminator included. */
#define BLESS_NEXT_MAX MANIFEST_PATH_MAX

/* Decodes the raw variable bytes into a path, or refuses them.
 *
 * UNTRUSTED INPUT, and more so than the manifest: this comes out of NVRAM,
 * which anything with boot services on the machine may write, including
 * something that has never seen this stick. So it is decoded, never believed:
 * the bytes must be printable ASCII, non-empty, and short enough to be a path
 * a record could carry, with at most one trailing NUL (the loader writes one;
 * a tool that did not is not wrong). Anything else is BLESS_ERR_VALUE and the
 * marker is discarded. The result is still only a NAME at this point: whether
 * it may be acted on is bless_next_names plus bless_hit_action, against
 * records read from the stick.
 *
 * out is NUL-terminated on success and left empty otherwise. */
int bless_next_decode(const void *buf, size_t len, char *out, size_t cap);

/* Does this menu entry's path name the file the marker names?
 *
 * path is the entry's path relative to the volume root, as the scan built it,
 * exactly as bless_cache_lookup takes it. Nonzero when they are the same file
 * by manifest_path_eq's rule. This is the only way a marker value is ever
 * turned into an entry: it is matched against paths the scan found, never used
 * to index anything, so a hostile value can name a file that is not there and
 * nothing else. */
int bless_next_names(const uint16_t *path, const char *name);

/* ---- reserved marker values (DESIGN Section 6.3.1) ----
 *
 * Everything above treats the marker as a path on p2. One target is not a file
 * at all: the personal OS is a pinned menu entry, not something the scan found,
 * so "boot straight back into the personal OS" needs a value that names it
 * without naming a path.
 *
 * The leading ':' is what makes that unambiguous, and it is chosen rather than
 * decorative: exFAT forbids ':' in a filename, so no file on p2 can ever
 * produce a path containing one, and a reserved value can therefore never
 * shadow a real entry. A '@' prefix would have read more nicely and been
 * wrong - a file called "@personal" is perfectly legal and would have been
 * silently unbootable.
 *
 * Reserved values are still UNTRUSTED input like any other marker value. This
 * one grants nothing that the menu does not: the personal-OS entry prompts for
 * the passphrase exactly as it would if the user had selected it by hand, so
 * naming it skips a keypress and nothing else. */
#define BLESS_NEXT_PERSONAL ":personal"

/* Nonzero when the decoded marker names the personal-OS entry rather than a
 * path. Callers must test this BEFORE matching against entry paths; a reserved
 * value will never match one, but testing after would mean a stale marker
 * reports "not on the stick any more", which is a confusing way to say
 * "unrecognised". */
int bless_next_is_personal(const char *name);

/* Nonzero when the decoded marker is in the reserved namespace at all, i.e.
 * begins with ':'. A reserved value this build does not recognise is a value
 * from a newer loader or a typo, and either way it must be refused rather than
 * looked up as a path. */
int bless_next_is_reserved(const char *name);

#endif /* FALCON_BLESS_CORE_H */
