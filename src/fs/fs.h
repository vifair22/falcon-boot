/* airies-boot: the filesystem driver registry (docs/DESIGN.md Section 9).
 *
 * One vtable over every read-only filesystem driver in the tree, so that a
 * consumer asks "what is on this volume" rather than naming exFAT or ext4,
 * and a new filesystem is one driver plus one registry row. DESIGN Section 9
 * fixes the shape: {name, probe, mount, dir_open_root, dir_open, dir_next,
 * file_open, file_read, extents}, registered in a static table, with extents
 * an OPTIONAL capability (a driver that cannot resolve a file to its physical
 * LBAs still serves the menu and the boot paths).
 *
 * Three problems this header exists to solve, and the answers taken:
 *
 * 1. Storage. Each driver has its own context types, all of them multi-
 *    kilobyte (a sector or block cache dominates) and none of them
 *    allocatable: there is no allocator in EFI-land that this code may use,
 *    and every driver in the tree is caller-allocates by contract. So the
 *    caller declares an fs_vol / fs_dir / fs_file, which is an ops pointer
 *    plus an opaque byte blob sized to the largest registered driver's
 *    context, and the vtable functions cast the blob to their own type. The
 *    sizes below are checked against every driver by a static assertion in
 *    fs.c, so adding a driver with a fatter context is a compile error rather
 *    than a stack smash.
 *
 * 2. Names. exFAT hands back UCS-2 and ext4 hands back bytes, and the
 *    consumers want both: the menu builds CHAR16 labels out of p2 paths, and
 *    the personal-OS path matches UKI names on p3. The unified dirent carries
 *    ONE canonical form, UCS-2, because it is the only one of the two that is
 *    lossless in both directions here: an ext4 name is widened byte by byte
 *    (so byte 0xC3 becomes U+00C3) and fs_name_to_bytes narrows it back to
 *    exactly the bytes the directory block held, while the reverse choice
 *    would have had to invent an encoding for exFAT's non-ASCII units. UCS-2
 *    is also what the console and the UEFI file paths take, which is where
 *    most of these names are going. A consumer that needs the on-disk bytes
 *    of an ext4 name (there is none today; the loader compares names and
 *    prints them) calls fs_name_to_bytes and gets them back unchanged.
 *
 *    Widening is not a character-set conversion and does not pretend to be:
 *    a UTF-8 ext4 name renders as mojibake if printed verbatim, which is why
 *    the display path sanitises to '?' exactly as it did before. What matters
 *    is that the round trip is exact, so nothing is lost on the way to the
 *    driver call that reopens the file.
 *
 * 3. Status codes. Each driver keeps its own enum, unrenumbered: the driver
 *    test suites pin those values and they are the right answers in the
 *    driver's own vocabulary. The registry maps them onto the FS_* set below,
 *    one table per driver in fs.c, and every driver code has a mapping (the
 *    fs test group enumerates them). The union is deliberate: a consumer that
 *    reports "the volume is outside what this loader supports" wants
 *    FS_ERR_UNSUPPORTED whether an ext4 feature bit or an exFAT VBR field
 *    produced it.
 */

#ifndef AIRIES_FS_H
#define AIRIES_FS_H

#include <stdint.h>
#include <stddef.h>

#include <part/part.h>

/* Status codes, repo convention: OK zero, errors negative. Each is the
 * meaning shared by whichever drivers can produce it; see fs.c for the
 * per-driver mapping tables. */
enum {
    FS_OK                 =  0,
    FS_ERR_IO             = -1,  /* read callback failed */
    FS_ERR_BAD_SUPER      = -2,  /* not this filesystem, or unusable super */
    FS_ERR_CORRUPT        = -3,  /* on-disk structure violates the format */
    FS_ERR_BAD_ARG        = -4,
    FS_ERR_RANGE          = -5,  /* read outside the file's valid data */
    FS_ERR_NOSPACE        = -6,  /* caller's array too small */
    FS_ERR_UNSUPPORTED    = -7,  /* feature outside the driver's set */
    FS_ERR_NEEDS_RECOVERY = -8,  /* journal replay pending; would read stale */
    FS_ERR_DIRTY          = -9,  /* not cleanly unmounted, or errors flagged */
    FS_ERR_CSUM           = -10, /* metadata checksum mismatch */
    FS_ERR_INLINE         = -11, /* data lives in the inode; unsupported */
    FS_ERR_SYMLINK        = -12, /* not a byte stream to read */
    FS_ERR_HOLE           = -13, /* sparse hole or uninitialized extent */
    FS_ERR_NO_CAPABILITY  = -14  /* the driver does not implement this call;
                                    today that is `extents` and nothing else */
};

/* Read len bytes at byte offset off (volume-relative). Returns 0 on success,
 * nonzero on failure. Identical in shape to every driver's own callback,
 * because it IS every driver's own callback: the registry casts rather than
 * wraps, so a read costs nothing extra for going through the vtable. */
typedef int (*fs_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

/* UCS-2 units. 255 is both exFAT's limit and ext4's (which counts bytes, and
 * a byte-widened name is never longer than the byte count). */
#define FS_NAME_MAX 255u

/* Opaque per-object storage, sized for the largest registered driver.
 *
 * 4352 = a 4 KiB sector/block cache plus 256 bytes of bookkeeping, which
 * covers every context in the tree today (the fattest is ext4_vol at 4248)
 * with room for a driver that carries a slightly wider header. fs.c asserts
 * each driver fits; a future driver that wants more changes these numbers and
 * everything that declares one of these objects grows with it. */
#define FS_VOL_SIZE  4352u
#define FS_DIR_SIZE  4352u
#define FS_FILE_SIZE 4352u

/* Driver-private bytes carried in a dirent: what dir_open / file_open /
 * extents need to reopen the object, minus the name, which the unified dirent
 * already carries. exFAT needs a cluster and a chain flag, ext4 an inode
 * number and a file type; 24 bytes leaves room for a driver that needs a
 * 64-bit locator and a flag or two. */
#define FS_DIRENT_PRIV_SIZE 24u

struct fs_ops;

/* One directory entry, driver-independent.
 *
 * The struct is copied around freely (the menu sorts a parallel array of
 * them), so it holds no pointers into itself and nothing that outlives the
 * volume it came from. It is handed back to dir_open/file_open/extents of the
 * SAME driver that produced it; the ops pointer it came through is the
 * caller's to keep, which in practice is the fs_vol it iterated. */
typedef struct fs_dirent {
    uint16_t name[FS_NAME_MAX + 1]; /* UCS-2, NUL-terminated */
    uint16_t name_len;              /* units, excluding the terminator */
    uint64_t size;                  /* valid data length; 0 for directories
                                       whose driver does not report one */
    uint8_t  is_dir;
    uint8_t  is_regular;            /* a plain file, as opposed to a device
                                       node or symlink; exFAT has no such
                                       thing, so there it is !is_dir */
    union {
        uint64_t align_;
        unsigned char b[FS_DIRENT_PRIV_SIZE];
    } priv;
} fs_dirent;

/* One physically contiguous run of a file, in BYTES relative to the start of
 * the volume. Same contract as exfat_extent, which is what it wraps: the
 * extents cover the file's valid data length exactly. */
typedef struct fs_extent {
    uint64_t vol_off;
    uint64_t len;
} fs_extent;

typedef struct fs_vol {
    const struct fs_ops *ops;   /* NULL until a successful fs_mount */
    union {
        uint64_t align_;
        unsigned char b[FS_VOL_SIZE];
    } priv;
} fs_vol;

typedef struct fs_dir {
    const struct fs_ops *ops;
    union {
        uint64_t align_;
        unsigned char b[FS_DIR_SIZE];
    } priv;
} fs_dir;

typedef struct fs_file {
    const struct fs_ops *ops;
    union {
        uint64_t align_;
        unsigned char b[FS_FILE_SIZE];
    } priv;
} fs_file;

/* Which role's partition a driver is the primary answer for, and the value a
 * caller passes when it has no idea. The two role values are src/part's, so
 * a discovery row's role and an identification hint are the same number. */
typedef enum {
    FS_HINT_NONE  = -1,
    FS_HINT_ISO   = (int)PART_ROLE_ISO,
    FS_HINT_SHELF = (int)PART_ROLE_SHELF
} fs_hint;

typedef struct fs_ops {
    const char *name;           /* stable identifier: "exfat", "ext4" */
    fs_hint     role;           /* the role this driver is tried first for */

    /* Cheap magic check, one read. FS_OK means "worth mounting", never
     * "valid": validation is mount's job and mount is allowed to refuse what
     * probe accepted (an ext4 with an unsupported feature bit, say). */
    int (*probe)(fs_read_fn read, void *ctx);

    int (*mount)(fs_vol *v, fs_read_fn read, void *ctx);
    int (*dir_open_root)(fs_vol *v, fs_dir *d);
    int (*dir_open)(fs_vol *v, fs_dir *d, const fs_dirent *e);
    /* 1 = entry produced, 0 = end of directory, negative = FS_ERR_*. */
    int (*dir_next)(fs_dir *d, fs_dirent *out);
    int (*file_open)(fs_vol *v, fs_file *f, const fs_dirent *e);
    int (*file_read)(fs_file *f, uint64_t off, uint64_t len, void *buf);
    uint64_t (*file_size)(const fs_file *f);

    /* OPTIONAL capability (DESIGN Section 9). NULL when the driver cannot
     * resolve a file to physical ranges; the wrapper then answers
     * FS_ERR_NO_CAPABILITY, which is the honest degradation rather than a
     * weird failure. ext4 is NULL today: it has no extent-map API, and the
     * injection framework that wants one only ever asks about p2. */
    int (*extents)(fs_vol *v, const fs_dirent *e, fs_extent *out, size_t cap,
                   size_t *count);
} fs_ops;

/* ---- the registry ---- */

size_t fs_driver_count(void);
const fs_ops *fs_driver_at(size_t i);
/* Exact match on fs_ops::name, or NULL. */
const fs_ops *fs_by_name(const char *name);

/* Identify what is on the volume: the role-appropriate driver is probed
 * first, then every other registered driver in table order. Returns the
 * driver that claimed it, or NULL when none did (which includes an encrypted
 * volume, whose bytes are nobody's filesystem until it is opened).
 *
 * Probe only. The volume is not mounted and nothing is validated, so a caller
 * that acts on the answer still checks fs_mount's status. */
const fs_ops *fs_identify(fs_read_fn read, void *ctx, fs_hint hint);

/* ---- the operations ---- */

/* Mounts with a driver the caller already has (from fs_identify, usually).
 * On success v->ops is set and every call below dispatches through it; on
 * failure v->ops is left NULL so a half-mounted volume cannot be used. */
int fs_mount(fs_vol *v, const fs_ops *ops, fs_read_fn read, void *ctx);

int fs_dir_open_root(fs_vol *v, fs_dir *d);
int fs_dir_open(fs_vol *v, fs_dir *d, const fs_dirent *e);
int fs_dir_next(fs_dir *d, fs_dirent *out);
int fs_file_open(fs_vol *v, fs_file *f, const fs_dirent *e);
int fs_file_read(fs_file *f, uint64_t off, uint64_t len, void *buf);
uint64_t fs_file_size(const fs_file *f);

/* Nonzero when this volume's driver implements the extent map. */
int fs_has_extents(const fs_vol *v);
/* FS_ERR_NO_CAPABILITY when it does not; otherwise exactly the driver's
 * contract, including FS_ERR_NOSPACE with *count set to what was needed. */
int fs_extents(fs_vol *v, const fs_dirent *e, fs_extent *out, size_t cap,
               size_t *count);

/* The mounted driver's name, or "none" for an unmounted volume. For messages
 * and for `mkstick status`; nothing branches on it. */
const char *fs_vol_name(const fs_vol *v);

/* ---- name conversion at the edges ---- */

/* Narrows a dirent name back to bytes. Units above 0xFF cannot have come from
 * a byte-named filesystem and become '?', which is a display answer rather
 * than a round trip; for an ext4-sourced name this reproduces the directory
 * block's bytes exactly. Always NUL-terminates within cap; returns the number
 * of bytes written, excluding the terminator. */
size_t fs_name_to_bytes(const uint16_t *name, uint8_t *out, size_t cap);

/* The widening a byte-named driver's dir_next performs, exposed because the
 * round trip is a property worth testing directly. */
size_t fs_name_from_bytes(const uint8_t *name, size_t len, uint16_t *out,
                          size_t cap);

/* ASCII-case-insensitive comparisons against narrow literals, which is what
 * both consumers do with these names. Kept here rather than in src/lib
 * because src/lib/string.h is the freestanding build's and the host tests
 * deliberately shim it away. */
int fs_name_eq_ascii(const uint16_t *name, const char *ascii);
int fs_name_ends_ascii(const uint16_t *name, const char *suffix);
/* Sort order: ASCII-case-insensitive, then length. */
int fs_name_cmp(const uint16_t *a, const uint16_t *b);

/* ---- the registered drivers ----
 *
 * Consumers select through fs_identify or fs_by_name and never name a driver
 * (DESIGN Section 9). These are exported for the registry table itself and
 * for the test group that proves each adapter's status mapping is total. */
extern const fs_ops fs_ops_exfat;
extern const fs_ops fs_ops_ext4;

/* The mapping tables, for the same reason: a test enumerates every value of
 * each driver's enum and asserts it maps. An unmapped code maps to
 * FS_ERR_CORRUPT, which is the conservative answer, so a driver that grows a
 * status without a row here degrades rather than lies. */
int fs_status_from_exfat(int rc);
int fs_status_from_ext4(int rc);

#endif /* AIRIES_FS_H */
