/* falcon-core: the identity record (docs/DESIGN.md Section 3.1).
 *
 * Every partition falcon made carries one, and the record, not the
 * partition table, says what the partition is: its role and the install it
 * belongs to. A loader starts from the partition it booted from, reads its
 * install ID, and takes only partitions carrying the same ID.
 *
 * Three encodings of one record:
 *
 *   - binary, 32 bytes at byte IDENT_BIN_OFF of a FAT boot sector (p1, a
 *     station partition, jump's floppy): ident_decode_bin / encode_bin;
 *   - text, one line "falcon 1 <role> <32 hex>" in a /.falcon file (exFAT
 *     p2, a plaintext ext4 shelf): ident_decode_text / encode_text;
 *   - a LUKS2 token of type "falcon" (p3), whose JSON carries "version",
 *     "role" and "install_id"; luks/luks.h reads the token and hands its
 *     role and install_id strings to ident_from_strings.
 *
 * EFI-free, allocation-free, no .bss (stage 2 links the binary half).
 */

#ifndef FALCON_IDENT_H
#define FALCON_IDENT_H

#include <stddef.h>
#include <stdint.h>

#define IDENT_BIN_OFF   0x1DCu   /* clears jump's floppy stage 1 (ends 0x1D9)
                                    and its stage 2 length word at 0x1FC */
#define IDENT_BIN_LEN   32u
#define IDENT_ID_LEN    16u
#define IDENT_VERSION   1u
#define IDENT_TEXT_MAX  64u      /* "falcon 1 library <32 hex>\n" and a NUL */
#define IDENT_FILE      "/.falcon"

typedef enum {
    IDENT_ROLE_BOOT    = 1,  /* FAT: BIOS stage 3 and the marker; the ESP */
    IDENT_ROLE_LIBRARY = 2,  /* exFAT p2 */
    IDENT_ROLE_SHELF   = 3   /* p3, LUKS2 or plaintext */
} ident_role;

typedef struct {
    uint8_t    version;
    ident_role role;
    uint16_t   stage3_sector;   /* BOOT: stage 3's first sector, relative to
                                   the partition start; 0 otherwise */
    uint16_t   marker_sector;   /* BOOT: the next-boot marker's sector,
                                   relative likewise; 0 otherwise */
    uint8_t    install_id[IDENT_ID_LEN];
} ident_rec;

enum {
    IDENT_OK         =  0,
    IDENT_ERR_NONE   = -1,   /* no record here (magic absent) */
    IDENT_ERR_BAD    = -2,   /* a record, but its CRC, version or fields
                                are wrong */
    IDENT_ERR_ARG    = -3
};

/* Binary: magic "FLCN", version, role, stage3 (LE16), marker (LE16),
 * reserved (2, zero), install ID (16), CRC-32 (LE32, gzip's) over the 28
 * bytes before it. `sector` is a whole boot sector (at least 512 bytes). */
int  ident_decode_bin(const uint8_t *sector, ident_rec *out);
void ident_encode_bin(const ident_rec *r, uint8_t *sector);

/* Text: "falcon <version> <boot|library|shelf> <32 lowercase hex>", one
 * line, an optional trailing newline, nothing else. Strict: anything out of
 * place is IDENT_ERR_BAD. encode writes the line with its newline and a NUL
 * and returns its length without the NUL. */
int    ident_decode_text(const char *buf, size_t len, ident_rec *out);
size_t ident_encode_text(const ident_rec *r, char *buf, size_t cap);

/* From the LUKS2 token's decoded fields: version number, role and install
 * ID strings (not NUL-terminated; lengths given). */
int ident_from_strings(uint32_t version, const char *role, size_t role_len,
                       const char *id_hex, size_t id_len, ident_rec *out);

/* Nonzero when both carry the same install ID. */
int ident_same_install(const ident_rec *a, const ident_rec *b);

const char *ident_role_name(ident_role role);

#endif /* FALCON_IDENT_H */
