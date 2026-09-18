/* falcon-boot-sign prepare: the decisions, with no I/O in them.
 *
 * Preparation (DESIGN Sections 6.3 and 6.3.1) is mostly subprocess work on a
 * 600 MB file, and almost none of what can go wrong is in the subprocess half.
 * What can go wrong is the arithmetic and the judgement: does the patched WIM
 * still fit the extent it has to be served over, is this ISO already prepared,
 * what exactly goes in the manifest record, and what does the WinPE-side
 * config say. So those four live here, take values rather than files, and are
 * tested in falcon-cli/tests/prepcore_test.c without an ISO, a stick or wimlib.
 *
 * The split is the same one falcon-efi/src/bless draws between bless.c
 * (firmware, I/O) and blesscore.c (records, decisions), and for the same
 * reason.
 */

#ifndef FALCON_CLI_SIGN_PREPCORE_H
#define FALCON_CLI_SIGN_PREPCORE_H

#include <stdint.h>
#include <stddef.h>

#include <manifest/manifest.h>

/* The reserved directory prepared sidecars live under, and the one file the
 * MVP patch shape stages there. Both are wire format: the loader resolves a
 * record's splice names inside p2:/falcon/prepared/<sha256>/, so renaming
 * either of these changes what an already-written manifest points at. */
#define PREP_DIR         "falcon/prepared"
#define PREP_BOOTWIM     "boot.wim"

/* Where in the patched WinPE image our three files land, and what the jump
 * binary looks for. These names are read from falcon-winjump/src/winjump.c
 * rather than invented: CFG_NAME and DEFAULT_SHELL there are what the runtime
 * half opens, so a disagreement here is a WinPE that boots to no media and says
 * nothing about why. */
#define PREP_SYSTEM32    "/Windows/System32"
#define PREP_SHELL       "winpeshl.exe"
#define PREP_SHELL_ORIG  "winpeshl_orig.exe"
#define PREP_CFG_NAME    "falcon-winjump.cfg"

/* What a lookup in the existing manifest says about an ISO we were asked to
 * prepare. Four answers rather than a boolean, because three of them mean
 * "do the work" for visibly different reasons and the operator should be told
 * which one applied. */
typedef enum {
    PREP_STATE_ABSENT = 0,  /* no record: never prepared on this stick */
    PREP_STATE_STALE,       /* a record, but the ISO's size has changed since */
    PREP_STATE_OTHER_KIND,  /* a record, but a blessed one: not our artifact */
    PREP_STATE_READY,       /* prepared, and the record still describes this
                             * exact file: nothing to do */
} prep_state;

/* The Section 8.1 lookup as preparation needs it: matched by path, then
 * confirmed by size, exactly as the loader will confirm it at menu time.
 * *out, when non-NULL, receives the matched record for every state but ABSENT
 * so the caller can name the sidecar folder it would have to re-check or
 * replace.
 *
 * Deliberately size-only, like the loader's own rule and unlike a checksum
 * comparison: the point of the fast path is that re-running prepare on an
 * already-prepared stick costs a manifest read rather than a SHA-256 of a
 * multi-gigabyte ISO. The checksum still governs correctness, because it is
 * what names the folder the record points at, and a replaced ISO of exactly
 * the same length is caught by the caller's check that that folder's files
 * are still there and still the right length. */
prep_state prep_lookup(const manifest_record *recs, size_t n,
                       const char *iso_rel, uint64_t cur_size,
                       const manifest_record **out);

/* The guard DESIGN Section 6.3 requires: the patched WIM is served over the
 * original's extent, so it may be padded up to that length and may never
 * exceed it. SGN_OK when it fits, SGN_ERR_NOSPACE with an actionable message
 * naming both sizes when it does not. Nothing else in the flow may make this
 * judgement, and nothing may truncate instead. */
int prep_fits(uint64_t patched_len, uint64_t orig_len);

/* Builds the prepared record of Section 8.1 and checks it against the
 * manifest's own rules before the caller commits to writing 600 MB.
 * iso_rel is the ISO's path relative to the p2 root with no leading
 * separator, sha_hex its 64-character lowercase digest. SGN_ERR_NOSPACE when
 * a value does not fit its field, SGN_ERR_FORMAT when the finished record is
 * not representable (a '#' or a non-ASCII byte in the path). */
int prep_build_record(const char *iso_rel, uint64_t iso_size,
                      const char *sha_hex, uint64_t splice_off,
                      uint64_t splice_len, const char *splice_name,
                      manifest_record *out);

/* Renders the WinPE-side config: ASCII key=value, the two keys
 * falcon-winjump/src/winjump.c reads. The path is written with no leading
 * separator because that is how it appears in the manifest, and winjump strips
 * one in any case. *used receives the byte count written (no NUL is counted).
 * SGN_ERR_NOSPACE when the buffer is too small. */
int prep_cfg_render(const char *iso_rel, uint64_t iso_size,
                    char *out, size_t cap, size_t *used);

#endif /* FALCON_CLI_SIGN_PREPCORE_H */
