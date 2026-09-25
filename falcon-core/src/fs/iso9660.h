/* falcon-boot: read-only ISO9660 with Rock Ridge and Joliet (docs/DESIGN.md
 * Sections 6.4 and 9).
 *
 * Sibling of falcon-core/src/fs/exfat.c and shaped the same way: freestanding,
 * EFI-free, every device access through a byte-addressed read callback, every
 * context caller-allocated. The loader runs it over an ISO file on p2 to find
 * a Linux kernel for direct boot (Section 6.4), and the sign tool runs it over
 * the same file to find what to wrap (Section 8.1); before this driver both
 * jobs went through bsdtar in userland and could not happen in the loader at
 * all.
 *
 * Which of an image's name spaces is read. A disc carries one primary tree
 * and may carry supplementary ones; the driver picks ONE tree at mount and
 * serves it for the life of the volume:
 *
 *   1. Rock Ridge on the primary tree, when the root's "." record opens with
 *      a SUSP "SP" entry. Names come from NM, file types from PX, relocated
 *      deep directories are followed through CL and hidden through RE. Every
 *      Linux live medium in the corpus has it.
 *   2. Joliet, when a supplementary descriptor carries one of the three UCS-2
 *      escape sequences. Names are UCS-2 big-endian.
 *   3. The plain primary tree otherwise, with ISO9660's ";1" version suffix
 *      and the empty extension's trailing "." removed.
 *
 * Case is left as recorded. ISO9660 names are upper case and Rock Ridge
 * names are whatever the author wrote, and matching is the caller's, which is
 * how every consumer in the tree already treats names (fs_name_eq_ascii).
 *
 * Supported: 2048-byte logical blocks (every ISO anyone makes; the field is
 * checked and anything else is refused), multi-extent files (a file over 4 GiB
 * is several consecutive records with the multi-extent flag, served here as
 * one file), and the extent map.
 *
 * Refused, each with its own status so a caller can say why:
 *   ISO9660_ERR_UNSUPPORTED  a logical block size other than 2048, an
 *                            interleaved file (file unit size nonzero), and a
 *                            zisofs-compressed file (Rock Ridge ZF), whose
 *                            bytes on disc are not the file's bytes.
 *   ISO9660_ERR_SYMLINK      a Rock Ridge symlink: not a byte stream.
 *
 * Nothing is trusted. The volume is an ISO someone dropped on p2, so every
 * length is checked against the buffer it indexes before it is used, loops
 * over continuation areas are bounded, and a record that does not fit its
 * sector is ISO9660_ERR_CORRUPT. What may boot is decided by signature
 * verification at load time (DESIGN Section 7), never by anything read here.
 */

#ifndef FALCON_ISO9660_H
#define FALCON_ISO9660_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    ISO9660_OK              = 0,
    ISO9660_ERR_IO          = -1, /* read callback failed */
    ISO9660_ERR_BAD_SUPER   = -2, /* no primary volume descriptor */
    ISO9660_ERR_CORRUPT     = -3, /* a structure violates the format */
    ISO9660_ERR_BAD_ARG     = -4,
    ISO9660_ERR_RANGE       = -5, /* read outside the file */
    ISO9660_ERR_NOSPACE     = -6, /* caller's array too small */
    ISO9660_ERR_UNSUPPORTED = -7, /* see the header comment */
    ISO9660_ERR_SYMLINK     = -8  /* not a byte stream to read */
};

typedef int (*iso9660_read_fn)(void *ctx, uint64_t off, uint64_t len,
                               void *buf);

#define ISO9660_BLOCK    2048u
#define ISO9660_NAME_MAX 255u

/* The most parts a multi-extent file may have. A part is at most 4 GiB less a
 * block, so this is about 64 GiB, far past any file on install media. */
#define ISO9660_MAX_PARTS 16u

/* Which name space the mounted tree is. */
typedef enum {
    ISO9660_NAMES_PLAIN  = 0,
    ISO9660_NAMES_JOLIET = 1,
    ISO9660_NAMES_RRIP   = 2
} iso9660_names;

typedef struct {
    iso9660_read_fn read;
    void *read_ctx;

    uint32_t root_lba;          /* the chosen tree's root directory */
    uint32_t root_len;
    iso9660_names names;
    uint8_t  su_skip;           /* SUSP SP's LEN_SKP */

    /* The primary descriptor's volume identifier, trailing spaces removed:
     * what dracut's root=live:CDLABEL= and udev's by-label name the medium
     * by, and so what a cmdline template's %LABEL% becomes. */
    char     label[33];

    /* One-block cache for continuation areas and the relocation lookups,
     * which interrupt a directory walk mid-sector and so cannot use the
     * walk's own buffer. ~0 marks it invalid. */
    uint64_t cache_off;
    uint8_t  cache[ISO9660_BLOCK];
} iso9660_vol;

typedef struct {
    uint16_t name[ISO9660_NAME_MAX + 1];
    uint16_t name_len;
    uint8_t  is_dir;
    uint8_t  is_regular;        /* 0 for a symlink or device node */
    uint8_t  compressed;        /* Rock Ridge ZF */
    uint8_t  parts;             /* extent records the file spans, 1.. */
    uint32_t lba;               /* first extent */
    uint64_t size;              /* all parts together */
    /* Where the first record sits, so a multi-extent file's later parts can
     * be found again at open time without carrying them all in a dirent. */
    uint32_t rec_lba;
    uint16_t rec_off;
} iso9660_dirent;

typedef struct {
    iso9660_vol *vol;
    uint32_t lba;
    uint32_t len;
    /* Byte offset into the directory extent. 64 bits although len is 32:
     * the walk steps to the end of the last block, which for a len within a
     * block of 4 GiB is 2^32, and a 32-bit pos wrapped there to 0 and walked
     * the directory again, forever. */
    uint64_t pos;
    uint64_t buf_off;           /* volume byte offset cached, ~0 invalid */
    uint8_t  buf[ISO9660_BLOCK];
} iso9660_dir;

typedef struct {
    uint32_t lba;
    uint32_t len;
} iso9660_part;

typedef struct {
    iso9660_vol *vol;
    uint64_t size;
    uint8_t  nparts;
    iso9660_part part[ISO9660_MAX_PARTS];
    uint64_t buf_off;           /* for requests that are not block-aligned */
    uint8_t  buf[ISO9660_BLOCK];
} iso9660_file;

typedef struct {
    uint64_t vol_off;
    uint64_t len;
} iso9660_extent;

int iso9660_mount(iso9660_vol *v, iso9660_read_fn read, void *read_ctx);
/* Mounts the primary (ISO 9660) tree with plain names, ignoring any Joliet or
 * Rock Ridge tree iso9660_mount would prefer: Windows' bootmgr reads a boot CD
 * through the primary descriptor, so a loader relocating a file for it must
 * edit the record there too. */
int iso9660_mount_primary(iso9660_vol *v, iso9660_read_fn read, void *read_ctx);
int iso9660_dir_open_root(iso9660_vol *v, iso9660_dir *d);
int iso9660_dir_open(iso9660_vol *v, iso9660_dir *d, const iso9660_dirent *e);
/* 1 = entry produced, 0 = end of directory, negative = error. "." and ".."
 * are never produced, nor is a directory Rock Ridge marks relocated (RE),
 * which appears once more at its real place through its CL record. */
int iso9660_dir_next(iso9660_dir *d, iso9660_dirent *out);

int iso9660_file_open(iso9660_vol *v, iso9660_file *f,
                      const iso9660_dirent *e);
/* Wholly inside the file or nothing, like every driver in the tree. */
int iso9660_file_read(iso9660_file *f, uint64_t off, uint64_t len, void *buf);

/* The file's physical runs in volume bytes, adjacent parts coalesced. Same
 * contract as exfat_file_extents: cap too small is ISO9660_ERR_NOSPACE with
 * *count set to what was needed. */
int iso9660_file_extents(iso9660_vol *v, const iso9660_dirent *e,
                         iso9660_extent *out, size_t cap, size_t *count);

/* Probe: one read, "CD001" at block 16. */
int iso9660_probe(iso9660_read_fn read, void *read_ctx);

#endif /* FALCON_ISO9660_H */
