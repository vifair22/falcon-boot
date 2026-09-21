/* falcon-boot: what is on a mounted volume, and how a menu moves through it
 * (DESIGN Section 6 stage 1). Shared by both loaders.
 *
 * The line: this module answers "what is here, and what is under it".
 * Rendering, keys, the pinned rows (personal OS, firmware setup) and the tag
 * columns of DESIGN Section 7.8 are the caller's, as is any per-entry state
 * it attaches. A row out of here is a directory or a file.
 *
 * Caller-allocated storage, as everywhere else in falcon-core. The arrays
 * outlive the walk: an entry is booted long after the walk that found it.
 */

#ifndef FALCON_SURVEY_H
#define FALCON_SURVEY_H

#include <stddef.h>
#include <stdint.h>

#include <fs/fs.h>

/* A path relative to the volume root, in UCS-2 units including the
 * terminator. Matches the EFI menu's row label. */
#define SURVEY_PATH_MAX 512u

/* How deep the walk goes. Each frame carries an fs_dir (~4.5 KB), so this is
 * what bounds the stack; falcon-bios' is at a fixed address with nothing
 * below it to grow into. */
#define SURVEY_DEPTH_MAX 4u

/* How much one volume's walk may read: entries produced, and directories
 * opened. Depth alone bounds nothing useful, because a directory entry can
 * name the directory it is in or an ancestor, and N such entries cost about
 * N^4 reads at this depth. These are far past what a stick filled by hand
 * holds. Past the directory budget the walk still lists files in the
 * directories it has open but opens no more; past the entry budget it stops.
 * Either way it keeps what it found. */
#define SURVEY_ENTRY_BUDGET 65536u
#define SURVEY_DIR_BUDGET   1024u

/* What the extension asked for. MEDIA is a .iso or .img: a virtual medium is
 * published and something else parses it (DESIGN Section 6). PAYLOAD is a
 * .efi: a PE that boots as itself (Section 7.2), where `media` means nothing.
 *
 * The extension says what a file is offered AS, never what may run. */
typedef enum {
    SURVEY_SHAPE_MEDIA = 0,
    SURVEY_SHAPE_PAYLOAD
} survey_shape;

/* CD is a .iso: 2048-byte blocks, the only size a firmware partition driver
 * looks for an El Torito catalog at. DISK is a .img: 512-byte sectors, so a
 * GPT/MBR path finds an ESP inside it. A parsing hint and nothing else. */
typedef enum {
    SURVEY_MEDIA_CD = 0,
    SURVEY_MEDIA_DISK
} survey_media;

/* Which extensions a caller wants back. falcon-bios passes ISO|IMG; nothing
 * on a BIOS machine runs a PE. */
#define SURVEY_ACCEPT_ISO 0x1u
#define SURVEY_ACCEPT_IMG 0x2u
#define SURVEY_ACCEPT_EFI 0x4u

typedef struct {
    /* Path relative to the volume root, every directory terminated by '/'.
     * The menu's label, and the sort key. */
    uint16_t     label[SURVEY_PATH_MAX];

    /* Where the file name starts in `label`: the length of the directory
     * prefix, 0 at the volume root. The view groups on it, which is why it
     * needs no second copy of the path. */
    size_t       dir_len;

    /* The caller's index for the volume this came off. Opaque here. */
    size_t       vol;

    survey_shape shape;
    survey_media media;     /* SURVEY_SHAPE_MEDIA only */

    /* Kept whole, not just the name: opening the file wants it back, and
     * re-walking to find it again costs a thunk per sector under BIOS. */
    fs_dirent    ent;
} survey_entry;

typedef struct {
    survey_entry *entries;   /* caller's storage, `cap` of them */
    size_t        cap;
    size_t        count;
    size_t        skipped;   /* found, but over `cap` or too long to name */
    unsigned      accept;    /* SURVEY_ACCEPT_* */

    /* The walk's path buffer. Not on the stack: a frame already carries an
     * fs_dir. */
    uint16_t      prefix[SURVEY_PATH_MAX];

    size_t        cur_vol;   /* the volume the walk in progress is reading */
    size_t        entries_left;  /* this volume's budget, see above */
    size_t        dirs_left;
} survey_list;

void survey_init(survey_list *l, survey_entry *storage, size_t cap,
                 unsigned accept);

/* Walk one mounted volume depth first. `vol_index` is recorded on each entry
 * and never interpreted. Returns nonzero only when the root would not open;
 * a volume holding no images is not an error. Entries accumulate across
 * calls, so several volumes feed one list. */
int survey_volume(survey_list *l, fs_vol *vol, size_t vol_index);

/* Sort by path, case-insensitively. The folder view reads out in this order.
 *
 * Sort BEFORE attaching anything of your own: a caller's per-entry state is a
 * parallel array indexed by position, and sorting after it is filled moves
 * one array and not the other, which boots the wrong image. */
void survey_sort(survey_list *l);

/* ---- the folder view ----
 *
 * The rows for folder `cur` (empty at the volume root), read out of the flat
 * list: subfolders first, then the files directly in it, both in list order
 * and so both sorted. The caller places its own pinned rows around them,
 * which is how it keeps control of what a mistimed Enter on row one does. */
typedef enum {
    SURVEY_ROW_DIR = 0,
    SURVEY_ROW_FILE
} survey_row_kind;

typedef struct {
    /* A file's name bare; a folder's with its '/' on the end, which is what
     * survey_enter appends. */
    uint16_t        label[SURVEY_PATH_MAX];
    survey_row_kind kind;
    size_t          index;   /* into survey_list::entries */
} survey_row;

size_t survey_view(const survey_list *l, const uint16_t *cur, size_t cur_len,
                   survey_row *rows, size_t cap);

/* Descend into the folder a SURVEY_ROW_DIR row names. Nonzero if the path
 * would not fit, which cannot happen for a row this module emitted. */
int survey_enter(uint16_t *cur, size_t *cur_len, const uint16_t *row);

/* Up one level: drop the last component and its separator. */
void survey_up(uint16_t *cur, size_t *cur_len);

#endif /* FALCON_SURVEY_H */
