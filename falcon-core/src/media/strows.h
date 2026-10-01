/* falcon-boot: the station's list, as data (docs/DESIGN.md Sections 12.3,
 * 12.7 and 12.8). Shared by both loaders.
 *
 * The firmware-free half of apps/station/efi_main.c and bios_main.c: the
 * rows the loader found, the names STATION.INI, the default and next boot
 * call them by (DESIGN Section 12.7), what a STATION.INI section changes or
 * adds, and the order the menu shows them in. Each
 * loader finds its rows and boots them; everything here is decided on bytes
 * a test can write, which is why it is its own file, and one file, so the
 * two stations cannot come to read one STATION.INI two ways.
 *
 * A row is found or added; either way it may carry two names: the one the
 * loader generates for what it found (`nxt-0003`, `gpt-1a2b3c4d-p1`,
 * `uki-arch`, `memtest`, `pos`) and the section name of the STATION.INI
 * entry that matched or made it. Both name it everywhere a name is taken.
 */

#ifndef FALCON_STROWS_H
#define FALCON_STROWS_H

#include <stddef.h>
#include <stdint.h>

#include <media/stationini.h>
#include <part/table.h>

/* Found and added together: a shelf's UKIs, NXT_MAX firmware entries, the
 * ESPs, memtest, Firmware Setup and the INI's own entries. */
#define STROWS_MAX       64u
#define STROWS_LABEL_MAX 128u

/* The words both stations give the rows they find (DESIGN Section
 * 12.10), as ASCII: a loader widens one by writing
 * u"" or L"" before it, and the installer writes them into STATION.INI and
 * `station status` prints them as they are, so all three say one thing. A
 * boot sector's loader is media/vbr.c's vbr_loader_name. */
#define STROWS_LABEL_POS       "Personal OS (encrypted)"
#define STROWS_LABEL_MEMTEST   "Memory test (memtest86+)"
#define STROWS_LABEL_BOOTCODE  "Boot code"
#define STROWS_LABEL_PARTITION "Partition"
#define STROWS_LABEL_BOOTMGR   "Windows Boot Manager"
#define STROWS_LABEL_FWSETUP   "Reboot to firmware setup"
/* station-legacy's routes, as jump labels them (DESIGN Section 12.10). */
#define STROWS_LABEL_CD        "CD drive"
#define STROWS_LABEL_USB       "USB stick"
#define STROWS_LABEL_CF        "CF card (PC Card)"

/* The encrypted shelf's row's generated name, the one row every station
 * finds that has no file, disk or entry to be named after. */
#define STROWS_NAME_POS "pos"

/* Windows Boot Manager's path, the default of a `method = efi` entry. */
#define STROWS_BOOTMGFW "\\EFI\\Microsoft\\Boot\\bootmgfw.efi"

typedef enum {
    STROW_POS = 1,   /* the encrypted shelf: the prompt, then its chooser */
    STROW_UKI,       /* a UKI by file name: a plaintext shelf's, or the
                        encrypted shelf's after the prompt (ref_none) */
    STROW_NXT,       /* a firmware boot entry: BootNext and a reset */
    STROW_ESP,       /* an image on an ESP, loaded by device path */
    STROW_MEMTEST,   /* memtest86+: on the station's own ESP (UEFI), or
                        MEMTEST.BIN in the station partition (BIOS) */
    STROW_FWSETUP,   /* the firmware's setup */
    STROW_VBR,       /* BIOS: a primary partition's boot sector, chained */
    STROW_MBR,       /* BIOS: another disk's MBR, chained */
    STROW_ROUTE      /* BIOS, station-legacy: one of jump's routes (DESIGN
                        Section 12.10); ref says which */
} strow_kind;

/* Which loader's rules strows_apply follows where the two differ: what a
 * section with `disk` means (a firmware entry or an ESP on UEFI, a chain on
 * BIOS) and whether `efi` means anything. */
typedef enum {
    STROWS_UEFI = 0,
    STROWS_BIOS = 1
} strows_fw;

typedef struct {
    strow_kind   kind;
    char         name[STATIONINI_NAME_MAX];     /* generated; "" none */
    char         section[STATIONINI_NAME_MAX];  /* the INI's; "" none */
    uint16_t     label[STROWS_LABEL_MAX];
    uint8_t      hidden;
    uint8_t      added;      /* made by a STATION.INI section */
    uint8_t      has_part;   /* disk and part say where it is */
    part_disk_id disk;
    uint32_t     part;       /* 0: any partition of the disk */
    uint8_t      own_esp;    /* ESP: the station's own ESP */
    uint8_t      no_swap;    /* VBR, MBR: swap = no, no drive swap */
    char         file[STATIONINI_PATH_MAX];     /* UKI: its file name;
                                                   ESP, NXT: the path on
                                                   the ESP, '\' separated */
    size_t       ref;        /* the caller's index: a pos image, an NXT row,
                                an ESP volume */
    uint8_t      ref_none;   /* ref names nothing yet: an added row, found
                                (or not) when it is booted */
} strow;

typedef struct {
    strow  row[STROWS_MAX];
    size_t n;
} strows;

void strows_init(strows *r);

/* A zeroed row of this kind at the end, or NULL when the list is full. */
strow *strows_add(strows *r, strow_kind kind);

/* The generated names (DESIGN Section 12.7). A disk's partition is
 * `gpt-<first 8 hex of the DiskGUID>-p<n>` or `mbr-<signature>-p<n>`, the
 * disk itself (part 0) the same without `-p<n>`; a firmware entry is
 * `nxt-<4 hex>`; a UKI is `uki-<its file name less .efi>`, every character
 * outside [A-Za-z0-9_-] made '-' and the whole cut to fit. */
void strows_name_part(char out[STATIONINI_NAME_MAX], const part_disk_id *d,
                      uint32_t part);
void strows_name_nxt(char out[STATIONINI_NAME_MAX], uint16_t option);
void strows_name_uki(char out[STATIONINI_NAME_MAX], const char *file);

/* "<what> (disk gpt:1a2b3c4d, partition 2)", or without the partition for
 * part 0: the label of something found on a disk, as the BIOS station
 * labels its rows. */
void strows_label_part(uint16_t *label, size_t cap, const uint16_t *what,
                       const part_disk_id *d, uint32_t part);

/* The same, in ASCII, for the installer and `station status`. */
void strows_label_part_ascii(char *label, size_t cap, const char *what,
                             const part_disk_id *d, uint32_t part);

/* Two paths on an ESP naming the same file: case-insensitive, and '/' and
 * '\' the same. */
int strows_path_eq(const char *a, const char *b);

/* Applies STATION.INI's entry sections (DESIGN Section 12.7):
 *
 *   - `uki`: the UKI row of that file, or a new one behind the prompt;
 *   - UEFI, `disk` with `method = efi` or `efi`: the ESP row on that disk
 *     (and partition) loading that path (Windows Boot Manager by default),
 *     or a new one;
 *   - UEFI, `disk` otherwise: the firmware entry on that disk (and
 *     partition), else an ESP row found there, else a new firmware-entry
 *     row that has no entry to boot (ref_none);
 *   - UEFI, `efi` alone: that path on the station's own ESP;
 *   - BIOS, `disk` with `part`: the VBR row of that disk and partition, or
 *     a new one (ref_none); `disk` alone: that disk's MBR row, or a new
 *     one; `efi` and `method = efi` are UEFI's, and a section they are all
 *     of is left out;
 *   - none of these: the found row whose generated name is the section's.
 *
 * The matched or new row takes the section's name, and its label and hide
 * when given, and on BIOS its swap. A section that matches nothing and says
 * nothing to boot is left out. */
void strows_apply(strows *r, const stationini *c, strows_fw fw);

/* The row a name names, by section name first and generated name second,
 * without regard to case; hidden rows included. NULL when none. */
strow *strows_find(strows *r, const char *name);

/* The rows the menu shows, as indices into r->row: `order`'s names first,
 * in its order, then every other shown row as found, and Firmware Setup
 * last whatever the order says (a mistimed Enter must not land on it).
 * Hidden rows are left out. Returns how many were written, at most cap. */
size_t strows_arrange(const strows *r, const stationini *c, size_t *view,
                      size_t cap);

/* The position in view of the default, when [general] names a row that is
 * shown: 1 with *sel set, else 0. */
int strows_default(const strows *r, const stationini *c, const size_t *view,
                   size_t n, size_t *sel);

#endif /* FALCON_STROWS_H */
