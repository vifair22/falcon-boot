/* falcon-boot-sign: the bless tool (docs/DESIGN.md Sections 7.5, 8 and 8.1).
 *
 * Wraps an ISO's kernel in a minimal signed UKI so the ISO can boot with
 * Secure Boot validation left on, and records the fact in the p2 manifest.
 * Ordinary Linux userland C. It reads ISOs with falcon-core's own ISO9660
 * and UDF drivers, the ones the loader uses (DESIGN Section 6.4), and shells
 * out to sbsign rather than reimplementing Authenticode signing, which is
 * the vendoring line DESIGN Section 8.1 draws. The UKI wrap itself is built here
 * (uki.c), not by ukify: DESIGN Section 8.1 says why. It never reimplements
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
 * the obvious candidates for a future falcon-cli/src/common when a second host
 * tool lands; they are prefixed sgn_ so hoisting them cannot collide with
 * anything a sibling tool already defines.
 */

#ifndef FALCON_CLI_SIGN_H
#define FALCON_CLI_SIGN_H

#include <stdint.h>
#include <stddef.h>

#include <media/family.h>
#include <media/bootcfg.h>

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

/* Is this program on PATH? Checked up front so a missing sbsign is one
 * clear message instead of a failure three steps into the flow. */
int sgn_have_program(const char *name);

/* The same lookup, reporting where: the PATH entry it was found in, joined to
 * the name, into out. 1 when found and it fits, 0 otherwise. For a program
 * that has to be run by its full path. */
int sgn_which(const char *name, char *out, size_t cap);

/* ------------------------------------------------------------ files */

/* Streams the file through SHA-256 (falcon-core/src/crypto/sha256.c, the
 * loader's own implementation, dogfooded here rather than shelling out to
 * sha256sum) and reports its size. ISOs are gigabytes: the file is never held
 * in memory. */
int sgn_sha256_file(const char *path, uint8_t digest[32], uint64_t *size);

/* Lowercase hex of a 32-byte digest into out, which must hold 65 bytes. */
void sgn_hex32(const uint8_t digest[32], char *out);

int sgn_read_file(const char *path, char **out, size_t *len, size_t max);
/* Writes through a sibling temporary and renames, so a crash mid-write
 * leaves the previous manifest intact rather than a truncated one. */
int sgn_write_file_atomic(const char *path, const void *buf, size_t len);
int sgn_mkdir_p(const char *path);
int sgn_file_exists(const char *path);

/* Size of a regular file. SGN_ERR_IO when it is not one. */
int sgn_file_size(const char *path, uint64_t *out);

/* Appends the whole of src to dst. For the initrds a boot entry loads one
 * after another, which the wrap carries as one concatenated section. */
int sgn_append_file(const char *dst, const char *src);

/* Appends len bytes to path, creating it when it does not exist. */
int sgn_append_bytes(const char *path, const void *data, size_t len);

/* Pads path with zeros to a multiple of 4 bytes, the alignment the kernel
 * wants between concatenated initrd archives. */
int sgn_pad4_file(const char *path);

/* Adds extra to the kernel's half of cmdline (cap bytes): at the end, or
 * before a "--" word, since the kernel hands what follows one to init
 * (Mint's entry ends "quiet splash --"). SGN_ERR_NOSPACE when it does not
 * fit, with cmdline untouched. */
int sgn_cmdline_insert(char *cmdline, size_t cap, const char *extra);

/* The last component of a path, or the whole string when there is none. */
const char *sgn_base_name(const char *p);

/* Copies src into a fixed manifest field, or fails with a message naming the
 * field, the size it needed and the cap it overran. */
int sgn_field_copy(char *dst, size_t cap, const char *src, const char *what);

/* The ISO's path as the booted system will see it: relative to the p2 root,
 * with a leading slash. Derived from where the file sits under stick, because
 * that is the only place it can be at boot; an ISO outside the stick is a copy
 * the user still has to make, and saying so beats recording a path that will
 * not resolve. An explicit override (the tool's --iso-path) wins outright. */
int sgn_derive_iso_path(const char *iso, const char *stick,
                        const char *override_path, char *out, size_t cap);

/* Short registry ids for the manifest's family= field. DESIGN Section 8.1
 * spells the Arch one "arch", and the loader matches on these strings, so they
 * are a wire format: add rows, never rename them. */
const char *sgn_family_slug(family_id id);

/* Filesystem UUID of the block device backing path, as blkid would report
 * it, rendered ready for a kernel cmdline tag ("1234-ABCD"). Reads
 * /proc/self/mountinfo and /dev/disk/by-uuid, so it needs no privileges and
 * no libblkid. SGN_ERR_IO when path is not on a block device the kernel can
 * name that way, which is the normal answer for a scratch directory. */
int sgn_fs_uuid_for_path(const char *path, char *out, size_t cap);

/* ------------------------------------------------------------ the ISO */

typedef struct iso_view iso_view;

/* Opens the ISO and indexes its whole tree with falcon-core's ISO9660 and UDF
 * drivers (DESIGN Section 6.4), through the filesystem registry, which reads
 * a bridge disc through its UDF side. No mount, no loop device, no root and
 * no helper process: members are read in process on demand. */
int iso_view_open(const char *path, iso_view **out);
void iso_view_close(iso_view *v);

/* The driver the view reads through: "udf", "iso9660", or "none" for a view
 * with no image behind it. For messages. */
const char *iso_view_reader(const iso_view *v);

/* Fills in a bootcfg_src backed by this view, so
 * falcon-core/src/media/bootcfg.c reads the ISO's own boot configuration
 * through the index. */
void iso_view_bootcfg_src(iso_view *v, bootcfg_src *src);

/* Fills in a family_probe backed by this view, so
 * falcon-core/src/media/family.c fingerprints a real ISO through the same two
 * callbacks the loader gives it for a firmware-mounted ESP. */
void iso_view_family_probe(iso_view *v, family_probe *fp);

/* The byte length the ISO's own directory entry records for a member. 1 with
 * *out set for a file, 0 for a directory, for a member pointer iso_view_find
 * did not hand out, and in a view with no sizes behind it. */
int iso_view_size(const iso_view *v, const char *member, uint64_t *out);

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

/* Where the initramfs that goes with `kernel` is: the family's own name for
 * it when the family has one, else the first initr* beside the kernel, which
 * is every other family in the table. NULL, with the error buffer set, when
 * neither is in the ISO. */
const char *iso_initrd_path(iso_view *v, family_id id, const char *kernel);

/* The ISO9660 volume label (the Primary Volume Descriptor's volume
 * identifier), trailing padding removed, into out. It goes into a signed
 * cmdline as %LABEL%, so anything but [A-Za-z0-9._-] is refused rather than
 * escaped: a label with a space in it would be a second kernel argument.
 * SGN_ERR_FORMAT for no PVD or an unusable label, SGN_ERR_IO when the file
 * cannot be read. */
int iso_volume_label(const char *path, char *out, size_t cap);

#endif /* FALCON_CLI_SIGN_H */
