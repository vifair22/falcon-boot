/* falcon-boot-sign: the bless tool (docs/DESIGN.md Sections 7.5, 8 and 8.1).
 *
 * Wraps an ISO's kernel in a minimal signed UKI so the ISO can boot with
 * Secure Boot validation left on, and records the fact in the p2 manifest.
 * Ordinary Linux userland C: it shells out to bsdtar, 7-Zip, ukify and
 * sbsign rather than reimplementing ISO9660, UDF, PE assembly or
 * Authenticode, which is the vendoring line DESIGN Section 8.1 draws. It never reimplements
 * signing and it never holds anything but the key file the user pointed it
 * at, which stays the caller's problem to keep encrypted at rest.
 *
 * Built as a portable static binary because the identical implementation
 * runs on the user's host and inside the on-stick signing appliance's
 * initramfs (Section 7.5), where there is no dynamic loader to speak of.
 *
 * The per-ISO "I vouch for this" confirmation of Section 7.5 is the
 * appliance's job, not this tool's: this tool is the non-interactive engine
 * both routes drive, and adding a prompt here would either be bypassed by
 * the appliance or turn every host-side scripted bless into a stuck pipe.
 *
 * The support routines below (error buffer, logging, subprocess exec) are
 * the obvious candidates for a future src/cli/common when a second host
 * tool lands; they are prefixed sgn_ so hoisting them cannot collide with
 * anything a sibling tool already defines.
 */

#ifndef FALCON_CLI_SIGN_H
#define FALCON_CLI_SIGN_H

#include <stdint.h>
#include <stddef.h>

#include <family/family.h>

/* Status codes, repo convention: OK zero, errors negative. Every one of
 * these is paired with a message in the error buffer, set by whichever
 * function was deepest when it went wrong. */
enum {
    SGN_OK           =  0,
    SGN_ERR_ARG      = -1,  /* bad usage or an argument that does not exist */
    SGN_ERR_IO       = -2,  /* a file could not be read or written */
    SGN_ERR_EXEC     = -3,  /* a helper program failed or is missing */
    SGN_ERR_FORMAT   = -4,  /* the ISO is not what it claimed to be */
    SGN_ERR_NOSPACE  = -5,  /* a fixed buffer or record array was too small */
    SGN_ERR_INTERNAL = -6,
};

/* ------------------------------------------------------------ errors */

/* Sets the thread-local error buffer. The deepest frame calls this; callers
 * propagate the code and leave the message alone. */
void sgn_set_error(int code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Same, with ": <strerror(errno)>" appended. For syscall failures, where
 * the reason is the errno and not anything the caller knows. */
void sgn_set_error_errno(int code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

const char *sgn_error(void);
int sgn_error_code(void);

/* ------------------------------------------------------------ logging */

typedef enum {
    SGN_LOG_DEBUG = 0,
    SGN_LOG_INFO  = 1,
    SGN_LOG_WARN  = 2,
    SGN_LOG_ERROR = 3,
} sgn_level;

/* Diagnostics go to stderr at every level; stdout carries only the result,
 * so `falcon-boot-sign ... | cat` stays parseable and 2>/dev/null stays
 * quiet without losing the answer. */
void sgn_log_set_level(sgn_level min);
void sgn_log(sgn_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------ exec */

/* Runs argv (NULL-terminated, argv[0] is looked up on PATH) via fork/exec.
 * Never system(): every argument reaches the child as its own argv element,
 * so an ISO named `; rm -rf ~` is a filename and not a command.
 *
 * sgn_run           child stdout discarded, stderr inherited
 * sgn_run_to_file   child stdout written to path (created/truncated)
 * sgn_run_capture   child stdout collected into *out (malloc'd, NUL
 *                   terminated, caller frees), capped at max bytes
 */
int sgn_run(char *const argv[]);
int sgn_run_to_file(char *const argv[], const char *path);
int sgn_run_capture(char *const argv[], char **out, size_t *len, size_t max);

/* execvp takes char *const argv[], not const char *const argv[]: a POSIX
 * wart older than const itself. Laundering through uintptr_t is how a const
 * path or a string literal gets into that array without tripping
 * -Wcast-qual. Nothing here or in the child writes through the result. */
static inline char *sgn_arg(const char *s)
{
    return (char *)(uintptr_t)s;
}

/* Is this program on PATH? Checked up front so a missing bsdtar is one
 * clear message instead of a failure three steps into the flow. */
int sgn_have_program(const char *name);

/* ------------------------------------------------------------ files */

/* Streams the file through SHA-256 (src/crypto/sha256.c, the loader's own
 * implementation, dogfooded here rather than shelling out to sha256sum) and
 * reports its size. ISOs are gigabytes: the file is never held in memory. */
int sgn_sha256_file(const char *path, uint8_t digest[32], uint64_t *size);

/* Lowercase hex of a 32-byte digest into out, which must hold 65 bytes. */
void sgn_hex32(const uint8_t digest[32], char *out);

int sgn_read_file(const char *path, char **out, size_t *len, size_t max);
/* Writes through a sibling temporary and renames, so a crash mid-write
 * leaves the previous manifest intact rather than a truncated one. */
int sgn_write_file_atomic(const char *path, const void *buf, size_t len);
int sgn_mkdir_p(const char *path);
int sgn_file_exists(const char *path);

/* Filesystem UUID of the block device backing path, as blkid would report
 * it, rendered ready for a kernel cmdline tag ("1234-ABCD"). Reads
 * /proc/self/mountinfo and /dev/disk/by-uuid, so it needs no privileges and
 * no libblkid. SGN_ERR_IO when path is not on a block device the kernel can
 * name that way, which is the normal answer for a scratch directory. */
int sgn_fs_uuid_for_path(const char *path, char *out, size_t cap);

/* ------------------------------------------------------------ the ISO */

typedef struct iso_view iso_view;

/* Opens the ISO by listing it: bsdtar for the ISO9660/Rock Ridge namespace,
 * 7-Zip for the UDF one, and the view is the union. No mount, no loop device
 * and no root: the listing is the directory tree, and individual members are
 * extracted on demand by whichever helper listed them.
 *
 * 7-Zip is a soft dependency. Without it the ISO9660 half still lists, which
 * is every Linux image in full; what is lost is UDF-only media, and open logs
 * a warning saying so. */
int iso_view_open(const char *path, iso_view **out);
void iso_view_close(iso_view *v);

/* Which helper read this image's UDF namespace, or NULL when none was
 * available. Callers use it to tell "this image has no such file" apart from
 * "nothing here could read the namespace the file would be in". */
const char *iso_view_udf_reader(const iso_view *v);

/* Fills in a family_probe backed by this view, so src/family/family.c
 * fingerprints a real ISO through the same two callbacks the loader gives
 * it for a firmware-mounted ESP. */
void iso_view_family_probe(iso_view *v, family_probe *fp);

/* Does the member exist? Paths are absolute and '/'-separated as
 * family.h specifies; a trailing '*' matches any suffix. On success with a
 * pattern, *match (when non-NULL) receives the concrete member path, valid
 * until the view is closed. */
int iso_view_find(iso_view *v, const char *path, const char **match);

/* Extracts one member to dst. */
int iso_view_extract(iso_view *v, const char *member, const char *dst);

/* Where this family keeps its kernel, resolved against what the ISO
 * actually contains. NULL when the family has no entry in the table or none
 * of its candidates are present, and the error buffer says which. */
const char *iso_kernel_path(iso_view *v, family_id id);

#endif /* FALCON_CLI_SIGN_H */
