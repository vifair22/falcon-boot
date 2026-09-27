/* falcon-bios: the BIOS station's menu rows (docs/DESIGN.md Sections 13.3,
 * 13.7 and 13.8), apart from the BIOS so the host tests reach them.
 *
 * The station finds entries on its own (the shelves' UKIs, primaries whose
 * boot sector is a known loader, other disks' MBRs, memtest86+) and
 * STATION.INI changes, hides, orders and adds to them. This is the part of
 * that with no disk in it: the names an autodetected entry gets
 * (docs/notes/station-rework.md section 10), the labels, the merge with the
 * parsed INI, and the order and default the menu is drawn with. A wrong
 * merge is the wrong system booted by a countdown nobody is watching, so it
 * is tested here rather than on a machine.
 *
 * Disks and shelves are the caller's, named here by index; a disk named in
 * the INI is turned into one by the caller's disk_of.
 */

#ifndef FALCON_BIOS_ROWS_H
#define FALCON_BIOS_ROWS_H

#include <stddef.h>
#include <stdint.h>

#include <part/table.h>
#include <media/stationini.h>

#define ROWS_MAX      40u
#define ROW_LABEL_MAX 80u   /* UCS-2 units, the terminator included */

typedef enum {
    ROW_SHELF = 1,   /* an encrypted shelf: unlock, then its chooser */
    ROW_UKI,         /* one UKI on a shelf, by file name */
    ROW_PART,        /* a partition's boot sector (chain.c) */
    ROW_DISK,        /* another disk's MBR (chain.c) */
    ROW_MEMTEST      /* MEMTEST.BIN in the station partition */
} row_kind;

typedef struct {
    row_kind kind;
    char     name[STATIONINI_NAME_MAX];   /* the autodetected name, or the
                                             section's for one it added */
    char     alias[STATIONINI_NAME_MAX];  /* the section that changed it */
    uint16_t label[ROW_LABEL_MAX];
    unsigned disk;                        /* PART, DISK */
    uint32_t part;                        /* PART: its number */
    unsigned shelf;                       /* SHELF, UKI */
    char     uki[STATIONINI_PATH_MAX];    /* UKI: the file, as named */
    uint8_t  swap;                        /* PART, DISK: 0 for swap = no */
    uint8_t  hide;
    uint8_t  added;                       /* by a section, not found */
} st_row;

typedef struct {
    st_row   row[ROWS_MAX];
    unsigned n;
    unsigned shown[ROWS_MAX];   /* the visible rows, in menu order */
    unsigned n_shown;
    int      def;               /* the default's place in shown, or -1 */
    unsigned timeout;           /* seconds; 0 when there is no default */
} st_rows;

void rows_init(st_rows *rs);

/* A new row of this kind and name; swap on, shown. A name already taken
 * gets "-2", "-3" and so on, so two disks with the same signature (0, on a
 * disk DOS partitioned) are still two rows. NULL when the list is full. */
st_row *rows_add(st_rows *rs, row_kind kind, const char *name);

/* The row a name (a section, an autodetected name, or a next-boot value)
 * means: the row of that name or alias, hidden or not, matched exactly as
 * stationini_find matches sections. -1 for none. */
int rows_find(const st_rows *rs, const char *name);

/* Autodetected names (docs/notes/station-rework.md section 10):
 * "mbr-<signature>-p<n>" or "gpt-<first 8 of the DiskGUID>-p<n>" for a
 * partition, without "-p<n>" for part 0 (the disk's MBR). */
void rows_name_disk(char *out, const part_disk_id *id, uint32_t part);

/* "uki-" and the file name without its .efi, each character that is not
 * [A-Za-z0-9_-] as '_', cut to fit. */
void rows_name_uki(char *out, const uint16_t *file);

/* "<what> (disk <id>, partition <n>)", or "<what> (disk <id>)" for part 0,
 * where <id> is the disk's STATION.INI spelling (part_disk_id_format). */
void rows_label_disk(uint16_t *out, const char *what, const part_disk_id *id,
                     uint32_t part);

/* An ASCII label into a row's UCS-2 one, cut to fit. */
void rows_label_ascii(uint16_t *out, const char *s);

/* Which of the caller's disks an INI `disk` names, or -1 when none attached
 * does. */
typedef int (*rows_disk_fn)(const part_disk_id *id, void *ctx);

/* What the merge could not use, for a serial line: the section and why. */
typedef void (*rows_note_fn)(const char *section, const char *why, void *ctx);

/* Applies every section of `c` (DESIGN Section 13.7). A section whose disk
 * and part, or whose uki, or else whose name, match a row changes it
 * (label, hide, swap) and becomes that row's alias; one that matches
 * nothing adds a row: disk (+part) a chain, uki a UKI on shelf 0 when
 * there is a shelf (`shelves` nonzero). A section this loader cannot use
 * (efi, a disk not attached, a uki with no shelf, a name matching nothing)
 * is noted and skipped. */
void rows_apply_ini(st_rows *rs, const stationini *c, unsigned shelves,
                    rows_disk_fn disk_of, rows_note_fn note, void *ctx);

/* The menu: `order`'s names first, then every other row as found, hidden
 * rows left out; the default is `default_name` when it names a row (shown
 * last if a section hid it), and only then is there a timeout. `c` may be
 * NULL (no STATION.INI). A default naming no row leaves def -1: the caller
 * says so. */
void rows_arrange(st_rows *rs, const stationini *c);

#endif /* FALCON_BIOS_ROWS_H */
