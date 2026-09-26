/* falcon-boot: PE/COFF header layout, read from a file's bytes.
 *
 * The one place the loader knows where things live in a PE image: the
 * optional header's CheckSum and SizeOfHeaders, the data directory array, and
 * the section table. Three consumers ask it questions about files they did not
 * build: the ISO survey (does the default loader carry a certificate table?),
 * the UKI labeller (where is .osrel?), and the boot-prediction code (the
 * certificate table, the .sbat section, and every range the Authenticode hash
 * covers). Before this module each of the first two carried its own copy of
 * the header walk, and the third would have been a fourth.
 *
 * FILE layout, never loaded layout: every offset here is a file offset, and
 * sections are found through PointerToRawData, not VirtualAddress. The images
 * in question are sitting on a disk, not mapped.
 *
 * WINDOWS. The callers read a small window off the front of a file and ask
 * where the rest of it is, because reading a 250 MB UKI to label a menu row is
 * not an option. So `len` (bytes in buf) and `total` (the file's length) are
 * kept apart throughout: the headers must be inside the window because they
 * are read here, while a range this module hands back is checked against the
 * FILE and is expected to lie outside the window. Conflating the two was a
 * real bug once (pos/osrel.c, 2026-09-15).
 *
 * EFI-free and allocation-free, like the filesystem drivers, so it is tested
 * off the firmware. Every input is untrusted: these bytes come off a p2 ISO
 * anyone could have dropped on the stick.
 */

#ifndef FALCON_PE_H
#define FALCON_PE_H

#include <stddef.h>
#include <stdint.h>

enum {
    PE_OK = 0,
    PE_ERR_BAD_ARG = -1,
    PE_ERR_NOT_PE = -2,     /* no MZ, no PE signature, or headers past the window */
    PE_ERR_SHORT = -3,      /* the answer lies past the window: read more */
    PE_ERR_ABSENT = -4,     /* no such directory entry or section */
    PE_ERR_RANGE = -5       /* the entry names bytes outside the file */
};

#define PE_MAGIC_PE32   0x10bu
#define PE_MAGIC_PE32P  0x20bu

/* The COFF Machine values this tree meets: an image's processor. */
#define PE_MACHINE_I386  0x014cu
#define PE_MACHINE_AMD64 0x8664u
#define PE_MACHINE_ARM64 0xaa64u

#define PE_DIR_SECURITY 4u          /* the certificate table */
#define PE_SECT_NAME    8u
#define PE_SECT_ENTRY   40u

typedef struct {
    size_t   len;           /* bytes of the file in buf */
    size_t   coff;          /* file offset of the COFF header */
    uint16_t machine;       /* the COFF Machine field, PE_MACHINE_* */
    size_t   opt;           /* file offset of the optional header */
    uint16_t opt_size;      /* SizeOfOptionalHeader */
    uint16_t magic;         /* PE_MAGIC_*, or anything else */
    uint16_t nsect;         /* NumberOfSections */
    size_t   sect_table;    /* file offset of the section table */
} pe_hdr;

typedef struct {
    char     name[PE_SECT_NAME + 1];    /* NUL-terminated copy */
    uint32_t raw_ptr;                   /* PointerToRawData */
    uint32_t raw_size;                  /* SizeOfRawData */
} pe_sect;

/* Reads the DOS stub pointer, the PE signature and the COFF header. Needs the
 * first lfanew + 24 bytes in buf and nothing more; the optional header and the
 * section table are only located here, and each accessor below checks its own
 * bytes against the window when it is asked.
 *
 * An unfamiliar optional-header magic is NOT an error: section lookups do not
 * depend on it, and the accessors that do (the data directories and the
 * header fields) refuse on their own. */
int pe_parse(const void *buf, size_t len, pe_hdr *out);

/* The optional header's field offsets differ between PE32 and PE32+ only after
 * ImageBase, and the four below straddle that line, so each is resolved per
 * magic. PE_ERR_NOT_PE for an unfamiliar magic, PE_ERR_SHORT when the field
 * lies past the window. */
int pe_checksum_off(const pe_hdr *h, size_t *off);
int pe_size_of_headers(const pe_hdr *h, const void *buf, uint32_t *out);
int pe_dir_count(const pe_hdr *h, const void *buf, uint32_t *out);

/* Data directory `idx`: its file offset (for the Authenticode hash, which
 * skips the certificate table's own entry) and its two fields. PE_ERR_ABSENT
 * when NumberOfRvaAndSizes stops short of it, which is an answer about the
 * image rather than a parse failure. For PE_DIR_SECURITY the "rva" is a FILE
 * offset, as the PE specification says. */
int pe_dir(const pe_hdr *h, const void *buf, uint32_t idx, size_t *entry_off,
           uint32_t *rva, uint32_t *size);

/* Section `i` of NumberOfSections. PE_ERR_SHORT when its header lies past the
 * window. */
int pe_section(const pe_hdr *h, const void *buf, uint16_t i, pe_sect *out);

/* Finds the section called `name` and returns its FILE range, checked against
 * `total`. The whole section table must be inside the window (PE_ERR_SHORT
 * otherwise): a lookup that stopped at the window's edge would answer "absent"
 * for a section that is merely further down the table. A section whose name
 * fills all eight bytes has no terminator and is compared in full. An empty
 * section is PE_ERR_ABSENT, since there is nothing in it to read. */
int pe_find_section(const pe_hdr *h, const void *buf, size_t total,
                    const char *name, size_t *off, size_t *size);

#endif /* FALCON_PE_H */
