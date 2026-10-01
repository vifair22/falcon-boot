/* falcon-core: STATION.INI, the station's configuration (docs/DESIGN.md
 * Section 12.7).
 *
 *     [general]
 *     default = win2k          ; a section name, or an autodetected entry's
 *     timeout = 5              ; seconds; 0 or absent: the menu waits
 *     order   = win2k, linux   ; names, first shown first; the rest follow
 *     poweroff = 300           ; seconds with no key before the machine is
 *                              ; powered off; 0 never; absent: 300
 *
 *     [win2k]                  ; one section per entry
 *     disk  = mbr:cdcccdcc     ; or gpt:<DiskGUID>
 *     part  = 1                ; absent: the disk's own MBR
 *     label = Windows 2000
 *     swap  = no               ; BIOS chain of another disk: no drive swap
 *
 *     [totp]                   ; the UEFI station's TOTP check (12.15)
 *     tpm   = 2.0              ; or 1.2
 *     index = 0x01800FA1       ; the seed's NV index
 *     pcrs  = 0, 2, 4, 7       ; what it is sealed to, 0 to 15
 *     key   = 000b...          ; the pinned TPM key: 2.0 the SRK's name
 *                              ; (34 bytes), 1.2 the SRK's modulus
 *     label = falcon-station:host   ; the authenticator's name for it
 *     srkauth = sha1-zero      ; 1.2 only: the SRK's secret is SHA-1 of
 *                              ; 20 zero bytes, not the zero bytes
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
 * Strict, as JUMP.INI is (DESIGN 13.7): a key it does not know, a value it
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
/* The idle power-off (DESIGN Section 7.9): a day at most; with no
 * `poweroff` the station waits TUI_IDLE_SECONDS, as every loader does. */
#define STATIONINI_POWEROFF_MAX     86400u
#define STATIONINI_FILE        "STATION.INI"
#define STATIONINI_TOTP_KEY_MAX 256u   /* a 2048-bit SRK modulus */
#define STATIONINI_TOTP_PCRS   16u     /* 0 to 15: the boot's PCRs */

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

/* [totp]: present only when the section parsed whole. tpm is 12 or 20;
 * pcrs a bit per PCR; key the pinned key's bytes (a 2.0 name is 34, its
 * nameAlg SHA-256 first; a 1.2 modulus 128 to 256, exponent 65537).
 * srk_sha1 (1.2 only): the SRK's usage secret is SHA-1 of the well-known
 * 20 zero bytes, as a TPM owned with a password of zeros has it (swtpm_setup
 * --srk-well-known), rather than the zero bytes themselves (Windows,
 * TrouSerS -z). Enrollment finds out which and says. */
typedef struct {
    uint8_t  present;
    uint8_t  tpm;
    uint8_t  srk_sha1;
    uint32_t index;
    uint32_t pcrs;
    uint8_t  key[STATIONINI_TOTP_KEY_MAX];
    uint16_t key_len;
    char     label[STATIONINI_LABEL_MAX];
} stationini_totp;

typedef struct {
    char             default_name[STATIONINI_NAME_MAX];  /* "" none */
    unsigned         timeout;                             /* seconds */
    unsigned         poweroff;           /* seconds, when poweroff_set */
    uint8_t          poweroff_set;
    char             order[STATIONINI_ORDER_MAX][STATIONINI_NAME_MAX];
    unsigned         n_order;
    stationini_entry entries[STATIONINI_ENTRIES_MAX];
    unsigned         n_entries;
    stationini_totp  totp;
    unsigned         err_line;                 /* 1-based; 0 no error */
    char             why[STATIONINI_WHY_MAX];  /* the error, a sentence */
} stationini;

/* Parses `len` bytes of `text` into `out`. 0 on success; -1 on an error,
 * with err_line and why set, default_name emptied, timeout 0, no poweroff
 * (the default then applies) and no [totp]. */
int stationini_parse(const char *text, size_t len, stationini *out);

/* The entry section of this name, ignoring case as the loaders do, or
 * NULL. */
const stationini_entry *stationini_find(const stationini *c,
                                        const char *name);

/* Writes a STATION.INI the parser accepts for `c` into buf (NUL-terminated);
 * returns its length, or 0 when it does not fit. The installer's writer. */
size_t stationini_format(const stationini *c, char *buf, size_t cap);

/* The [totp] section alone, header included, for t; 0 when t is not
 * present, would not parse back, or does not fit. */
size_t stationini_format_totp(const stationini_totp *t, char *buf,
                              size_t cap);

/* `text` with the section named `name` (its header to the next header or
 * the end) taken out, everything else byte for byte, comments included:
 * how mkstick replaces [totp] in a file a person may have edited. Returns
 * the length written (NUL-terminated), or (size_t)-1 when it does not
 * fit. */
size_t stationini_drop_section(const char *text, size_t len,
                               const char *name, char *out, size_t cap);

#endif /* FALCON_STATIONINI_H */
