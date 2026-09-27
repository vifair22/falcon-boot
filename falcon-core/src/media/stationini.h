/* falcon-core: STATION.INI, the station's configuration (docs/DESIGN.md
 * Section 13.7).
 *
 *     [general]
 *     default = win2k          ; a section name, or an autodetected entry's
 *     timeout = 5              ; seconds; 0 or absent: the menu waits
 *     order   = win2k, linux   ; names, first shown first; the rest follow
 *
 *     [win2k]                  ; one section per entry
 *     disk  = mbr:cdcccdcc     ; or gpt:<DiskGUID>
 *     part  = 1                ; absent: the disk's own MBR
 *     label = Windows 2000
 *     swap  = no               ; BIOS chain of another disk: no drive swap
 *
 * Keys of an entry section: disk, part, uki (a file on the shelf), efi (a
 * path on an ESP; with disk, that disk's ESP), method (firmware | efi,
 * UEFI Windows), swap (yes | no), label, hide (yes | no).
 *
 * A section whose disk and part (or uki, or efi) match an entry the loader
 * found changes that entry; one that matches nothing adds one. The kind of
 * an added entry follows from its keys: uki => Linux, efi => a UEFI
 * image, disk (+part) => a BIOS chain (on UEFI: the firmware entry, or the
 * disk's ESP bootmgfw.efi with method = efi).
 *
 * Strict, as JUMP.INI is (DESIGN 14.7): a key it does not know, a value it
 * cannot parse, a key given twice, a section given twice, a timeout with no
 * default, or a default naming no section is an error naming the line; the
 * result is then no default and no countdown (the entries still parse up to
 * the error and are kept). Keys and yes/no are matched without regard to
 * case; ';' or '#' starts a comment; CR before LF is ignored. Names are
 * [A-Za-z0-9_-], 1 to STATIONINI_NAME_MAX-1 characters.
 *
 * EFI-free, allocation-free, no .bss: the caller owns the result. One
 * parser for both loaders; jump keeps jumpini.c (its keys are jump's).
 */

#ifndef FALCON_STATIONINI_H
#define FALCON_STATIONINI_H

#include <stddef.h>
#include <stdint.h>

#include <part/table.h>

#define STATIONINI_NAME_MAX    32u
#define STATIONINI_PATH_MAX    128u
#define STATIONINI_LABEL_MAX   64u
#define STATIONINI_ENTRIES_MAX 32u
#define STATIONINI_ORDER_MAX   32u
#define STATIONINI_WHY_MAX     80u
#define STATIONINI_TIMEOUT_MAX 3600u
#define STATIONINI_FILE        "STATION.INI"

typedef enum {
    STATIONINI_TRI_UNSET = 0,
    STATIONINI_TRI_NO    = 1,
    STATIONINI_TRI_YES   = 2
} stationini_tri;

typedef enum {
    STATIONINI_METHOD_UNSET    = 0,
    STATIONINI_METHOD_FIRMWARE = 1,
    STATIONINI_METHOD_EFI      = 2
} stationini_method;

typedef struct {
    char              name[STATIONINI_NAME_MAX];
    uint8_t           has_disk;
    part_disk_id      disk;
    uint32_t          part;                    /* 0: the disk's MBR */
    char              uki[STATIONINI_PATH_MAX];  /* "" when absent */
    char              efi[STATIONINI_PATH_MAX];
    stationini_method method;
    stationini_tri    swap;
    char              label[STATIONINI_LABEL_MAX];
    stationini_tri    hide;
    unsigned          line;                    /* where the section began */
} stationini_entry;

typedef struct {
    char             default_name[STATIONINI_NAME_MAX];  /* "" none */
    unsigned         timeout;                             /* seconds */
    char             order[STATIONINI_ORDER_MAX][STATIONINI_NAME_MAX];
    unsigned         n_order;
    stationini_entry entries[STATIONINI_ENTRIES_MAX];
    unsigned         n_entries;
    unsigned         err_line;                 /* 1-based; 0 no error */
    char             why[STATIONINI_WHY_MAX];  /* the error, a sentence */
} stationini;

/* Parses `len` bytes of `text` into `out`. 0 on success; -1 on an error,
 * with err_line and why set, default_name emptied and timeout 0. */
int stationini_parse(const char *text, size_t len, stationini *out);

/* The entry section of this name, or NULL. */
const stationini_entry *stationini_find(const stationini *c,
                                        const char *name);

/* Writes a STATION.INI the parser accepts for `c` into buf (NUL-terminated);
 * returns its length, or 0 when it does not fit. The installer's writer. */
size_t stationini_format(const stationini *c, char *buf, size_t cap);

#endif /* FALCON_STATIONINI_H */
