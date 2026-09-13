/* airies-boot: the blessed-artifact manifest (docs/DESIGN.md Section 8.1).
 *
 * The manifest is the plain-ASCII index at p2:/airies/blessed/manifest that
 * tells the loader which ISO on p2 has a signed UKI wrap standing behind it.
 * One block per blessed ISO:
 *
 *   iso=Linux/archlinux-2026.01.01-x86_64.iso   # path relative to p2 root
 *   size=1538670592                             # byte size at bless time
 *   sha256=<64 lowercase hex>                   # ISO hash at bless time
 *   uki=archlinux-2026.01.01-x86_64.uki         # file under /airies/blessed/
 *   family=arch                                 # registry id (Section 6.2)
 *
 * Deliberately EFI-free and allocator-free, for the same reason the exFAT
 * driver and the family registry are: the reader runs in the loader against a
 * buffer the exFAT driver filled, and unchanged in a host process against a
 * string literal. The caller owns every byte of output.
 *
 * Security posture, so nobody hardens the wrong thing. The manifest is on
 * writable, unverified exFAT: an attacker with the stick can edit it freely.
 * Nothing it says is trusted. Everything it can influence is either signed
 * (the UKI, which shim verifies on load) or already-unverified data the same
 * attacker could edit directly. So this parser's job is not to detect
 * tampering, it is to never turn bad input into bad behaviour: it is total
 * over its input. Unknown keys are skipped, malformed records are skipped,
 * a truncated or corrupt file degrades to "fewer records" and an absent one
 * to "nothing is blessed". There is no input for which it reports an error
 * the loader has to handle, and none for which it reads out of bounds.
 *
 * Split across two translation units on purpose: manifest.c is the read side
 * and is all the loader links, manifest_write.c is the record-editing and
 * rendering half that only the bless tool (Section 8) needs.
 */

#ifndef AIRIES_MANIFEST_H
#define AIRIES_MANIFEST_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    MANIFEST_OK          = 0,
    MANIFEST_ERR_BAD_ARG = -1,
    /* Caller's array was too small; *count reports how many records the
     * buffer actually holds, exactly as exfat_dir_read does it. */
    MANIFEST_ERR_NOSPACE = -2,
    /* Writer only: a record that cannot be represented in the format. */
    MANIFEST_ERR_BAD_RECORD = -3,
};

/* Field caps. These bound a parsed record, so they are part of the format:
 * a record whose value does not fit is skipped rather than truncated,
 * because a truncated path is a path that matches the wrong file.
 *
 * MANIFEST_PATH_MAX covers the ISO's path relative to the p2 root. exFAT
 * itself allows far longer paths; 255 characters is the depth any
 * drag-and-drop library reaches and keeps a record near half a kilobyte, so
 * a loader-side array of a few dozen is still a sane allocation. */
#define MANIFEST_PATH_MAX   256u
#define MANIFEST_NAME_MAX   128u
#define MANIFEST_FAMILY_MAX  32u
#define MANIFEST_SHA256_HEX  64u

/* Largest size= a record may carry. INT64_MAX rather than UINT64_MAX: no
 * file is that large, every host API that will ever report this size returns
 * it in an off_t, and capping here means no caller has to think about what
 * happens when the value is converted to a signed type. */
#define MANIFEST_SIZE_MAX ((uint64_t)0x7FFFFFFFFFFFFFFFu)

/* One blessed ISO. Every string is NUL-terminated printable ASCII. */
typedef struct {
    char     iso[MANIFEST_PATH_MAX];         /* path relative to the p2 root */
    char     uki[MANIFEST_NAME_MAX];         /* file under /airies/blessed/ */
    char     family[MANIFEST_FAMILY_MAX];    /* registry id, may be empty */
    char     sha256[MANIFEST_SHA256_HEX + 1u]; /* lowercase hex, or empty */
    uint64_t size;                           /* ISO size at bless time */
    uint8_t  has_sha256;                     /* 0 when the field was absent */
} manifest_record;

/* What a menu entry is, per DESIGN Section 8.1: the loader labels an ISO
 * blessed only when the path matches AND the current size still agrees, and
 * boots a stale-blessed entry as if it were unblessed. Both facts come out
 * of one lookup so no caller can implement half the rule. */
typedef enum {
    MANIFEST_BLESS_NONE  = 0,  /* no record for this path */
    MANIFEST_BLESS_FRESH = 1,  /* record found, size agrees */
    MANIFEST_BLESS_STALE = 2,  /* record found, the ISO changed since */
} manifest_bless;

/* Parses the whole buffer. Never fails on content.
 *
 * Writes up to cap records to out and always sets *count to the number of
 * valid records the buffer holds, which may exceed cap; that case returns
 * MANIFEST_ERR_NOSPACE and the first cap records are still valid output.
 * MANIFEST_ERR_BAD_ARG is only ever about the arguments themselves (a NULL
 * buf with a nonzero len, a NULL count, a NULL out with a nonzero cap).
 *
 * Tolerances, all of which appear in real hand-edited files:
 *   - '#' starts a comment that runs to end of line, anywhere on the line.
 *   - CRLF and bare LF line endings; a missing final newline.
 *   - Leading and trailing whitespace around keys and values.
 *   - Unknown keys are ignored. Keys appearing before the first iso= are
 *     ignored, since they belong to no block.
 *   - Blank lines and lines with no '=' are ignored.
 *
 * A record is skipped, not an error, when: it has no uki=; it has no usable
 * size= (non-numeric, or above MANIFEST_SIZE_MAX); any value is too long for
 * its field; or any value contains a byte outside printable ASCII. That last
 * one is why the loader can compare paths as bytes at all, and it means an
 * ISO whose name is not ASCII cannot be blessed: see manifest_path_from_ucs2.
 * A malformed sha256= is dropped on its own (has_sha256 stays 0) and the
 * record survives, because the loader does not read that field and the bless
 * tool recomputes the hash anyway. */
int manifest_parse(const void *buf, size_t len,
                   manifest_record *out, size_t cap, size_t *count);

/* Path equality as the manifest means it, which is how exFAT means it:
 * ASCII-case-insensitive, '/' and '\\' interchangeable, repeated and
 * leading separators collapsed, one trailing separator ignored. Nonzero
 * when equal. */
int manifest_path_eq(const char *a, const char *b);

/* First record whose iso= matches, or NULL. */
const manifest_record *manifest_find(const manifest_record *recs, size_t n,
                                     const char *iso_path);

/* The Section 8.1 rule in one call. *out, when non-NULL, receives the
 * matched record (including on STALE, so the caller can say why). */
manifest_bless manifest_lookup(const manifest_record *recs, size_t n,
                               const char *iso_path, uint64_t cur_size,
                               const manifest_record **out);

/* Converts a UCS-2 path (what the exFAT driver hands back) into the ASCII
 * bytes the lookups above take. Separators are passed through unchanged, so
 * a backslash-separated exFAT path compares equal to a slash-separated
 * manifest path.
 *
 * Returns MANIFEST_ERR_BAD_ARG when a character is outside printable ASCII
 * or the buffer is too small, and the caller should treat that as "not
 * blessed": no manifest record can name such a path, because manifest_parse
 * skips records that contain one. */
int manifest_path_from_ucs2(const uint16_t *src, char *dst, size_t cap);

/* ---- writer half, manifest_write.c; not linked into the loader ---- */

/* Can this record be written out and read back unchanged? Enforces the same
 * rules manifest_parse enforces, plus the two the format itself imposes:
 * no '#' (it would become a comment) and no newline (it would split the
 * record). Nonzero when the record is representable. */
int manifest_record_valid(const manifest_record *r);

/* Idempotent insert: replaces the record whose iso= matches rec (by
 * manifest_path_eq, so re-blessing the same ISO updates in place and never
 * duplicates), otherwise appends. *n is updated on append.
 * MANIFEST_ERR_NOSPACE when an append would exceed cap. */
int manifest_upsert(manifest_record *recs, size_t *n, size_t cap,
                    const manifest_record *rec);

/* Renders records to ASCII text, ready to write to p2. *used receives the
 * byte count (no terminating NUL is written or counted; pass cap larger than
 * the expected output if you want to NUL-terminate it yourself).
 *
 * This is a rewrite from the parsed records, so any comment the previous
 * file carried beyond the header this emits is lost. Accepted deliberately:
 * preserving arbitrary interleaved comments across an in-place record
 * replacement means keeping a byte-level edit model of a file that nothing
 * but this tool is expected to hand-edit, and the alternative failure (a
 * comment silently attached to the wrong record after a re-bless) is worse
 * than losing it. */
int manifest_render(const manifest_record *recs, size_t n,
                    char *out, size_t cap, size_t *used);

#endif /* AIRIES_MANIFEST_H */
