#include <pe/pe.h>

#include <lib/string.h>

/* Every offset below is bounded against the window before it is read, and
 * every sum that could wrap is checked by subtraction. lfanew, the optional
 * header size and NumberOfSections are all attacker-controlled. */

#define DOS_MAGIC       0x5A4Du     /* "MZ" */
#define PE_LFANEW_OFF   0x3Cu
#define COFF_SIZE       24u         /* "PE\0\0" + the 20-byte COFF header */
#define COFF_NSECT      6u          /* from the PE signature */
#define COFF_OPTSIZE    20u

/* Optional-header offsets. CheckSum and SizeOfHeaders sit before the point
 * where PE32+ widens ImageBase and the stack/heap sizes, so they agree; the
 * directory count and the array itself do not. */
#define OPT_SIZE_OF_HEADERS 60u
#define OPT_CHECKSUM        64u
#define OPT_NRVA_PE32       92u
#define OPT_NRVA_PE32P      108u
#define OPT_DIRS_PE32       96u
#define OPT_DIRS_PE32P      112u

#define SECT_RAWSIZE    16u
#define SECT_RAWPTR     20u

/* Is [off, off + n) inside the window? */
static int in_window(const pe_hdr *h, size_t off, size_t n)
{
    return off <= h->len && n <= h->len - off;
}

int pe_parse(const void *buf, size_t len, pe_hdr *out)
{
    const uint8_t *b = buf;
    uint32_t lfanew;

    if (b == (const uint8_t *)0 || out == (pe_hdr *)0)
        return PE_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));

    if (len < PE_LFANEW_OFF + 4u || get_le16(b) != DOS_MAGIC)
        return PE_ERR_NOT_PE;
    lfanew = get_le32(b + PE_LFANEW_OFF);
    if (lfanew > len || len - lfanew < COFF_SIZE)
        return PE_ERR_NOT_PE;
    if (memcmp(b + lfanew, "PE\0\0", 4) != 0)
        return PE_ERR_NOT_PE;

    out->len = len;
    out->coff = (size_t)lfanew + 4u;
    out->opt = (size_t)lfanew + COFF_SIZE;
    out->nsect = get_le16(b + lfanew + COFF_NSECT);
    out->opt_size = get_le16(b + lfanew + COFF_OPTSIZE);
    out->sect_table = out->opt + out->opt_size;
    /* The magic is the optional header's first field. An image with no
     * optional header has no magic, which leaves it 0 and every accessor that
     * depends on it refusing. */
    if (out->opt_size >= 2u && in_window(out, out->opt, 2u))
        out->magic = get_le16(b + out->opt);
    return PE_OK;
}

/* A field of the optional header: known magic, inside SizeOfOptionalHeader,
 * and inside the window. */
static int opt_field(const pe_hdr *h, size_t field, size_t width, size_t *off)
{
    if (h->magic != PE_MAGIC_PE32 && h->magic != PE_MAGIC_PE32P)
        return PE_ERR_NOT_PE;
    if (field + width > h->opt_size)
        return PE_ERR_NOT_PE;
    if (!in_window(h, h->opt + field, width))
        return PE_ERR_SHORT;
    *off = h->opt + field;
    return PE_OK;
}

int pe_checksum_off(const pe_hdr *h, size_t *off)
{
    if (h == (const pe_hdr *)0 || off == (size_t *)0)
        return PE_ERR_BAD_ARG;
    return opt_field(h, OPT_CHECKSUM, 4u, off);
}

int pe_size_of_headers(const pe_hdr *h, const void *buf, uint32_t *out)
{
    size_t off;
    int rc;

    if (h == (const pe_hdr *)0 || buf == (const void *)0 ||
        out == (uint32_t *)0)
        return PE_ERR_BAD_ARG;
    rc = opt_field(h, OPT_SIZE_OF_HEADERS, 4u, &off);
    if (rc != PE_OK)
        return rc;
    *out = get_le32((const uint8_t *)buf + off);
    return PE_OK;
}

int pe_dir_count(const pe_hdr *h, const void *buf, uint32_t *out)
{
    size_t off;
    int rc;

    if (h == (const pe_hdr *)0 || buf == (const void *)0 ||
        out == (uint32_t *)0)
        return PE_ERR_BAD_ARG;
    rc = opt_field(h, h->magic == PE_MAGIC_PE32P ? OPT_NRVA_PE32P
                                                 : OPT_NRVA_PE32,
                   4u, &off);
    if (rc != PE_OK)
        return rc;
    *out = get_le32((const uint8_t *)buf + off);
    return PE_OK;
}

int pe_dir(const pe_hdr *h, const void *buf, uint32_t idx, size_t *entry_off,
           uint32_t *rva, uint32_t *size)
{
    uint32_t n;
    size_t off;
    int rc;

    if (entry_off == (size_t *)0 || rva == (uint32_t *)0 ||
        size == (uint32_t *)0)
        return PE_ERR_BAD_ARG;
    rc = pe_dir_count(h, buf, &n);
    if (rc != PE_OK)
        return rc;
    if (idx >= n)
        return PE_ERR_ABSENT;
    /* idx < n <= UINT32_MAX, so idx * 8 fits a size_t; whether the entry is
     * inside the optional header and the window is opt_field's question. */
    rc = opt_field(h, (h->magic == PE_MAGIC_PE32P ? OPT_DIRS_PE32P
                                                  : OPT_DIRS_PE32) +
                          (size_t)idx * 8u,
                   8u, &off);
    if (rc != PE_OK)
        return rc;
    *entry_off = off;
    *rva = get_le32((const uint8_t *)buf + off);
    *size = get_le32((const uint8_t *)buf + off + 4u);
    return PE_OK;
}

int pe_section(const pe_hdr *h, const void *buf, uint16_t i, pe_sect *out)
{
    const uint8_t *s;
    size_t off;

    if (h == (const pe_hdr *)0 || buf == (const void *)0 ||
        out == (pe_sect *)0)
        return PE_ERR_BAD_ARG;
    if (i >= h->nsect)
        return PE_ERR_ABSENT;
    off = h->sect_table + (size_t)i * PE_SECT_ENTRY;
    if (!in_window(h, off, PE_SECT_ENTRY))
        return PE_ERR_SHORT;
    s = (const uint8_t *)buf + off;
    memcpy(out->name, s, PE_SECT_NAME);
    out->name[PE_SECT_NAME] = '\0';
    out->raw_size = get_le32(s + SECT_RAWSIZE);
    out->raw_ptr = get_le32(s + SECT_RAWPTR);
    return PE_OK;
}

/* Section names are 8 bytes, NUL-padded rather than NUL-terminated, so a name
 * that fills the field has no terminator at all. A shorter `want` must be
 * followed by padding, so ".sbat" does not match ".sbatx". */
static int name_is(const char *field, const char *want)
{
    size_t i;

    for (i = 0; i < PE_SECT_NAME; i++) {
        if (want[i] == '\0') {
            for (; i < PE_SECT_NAME; i++) {
                if (field[i] != '\0')
                    return 0;
            }
            return 1;
        }
        if (field[i] != want[i])
            return 0;
    }
    return 1;
}

int pe_find_section(const pe_hdr *h, const void *buf, size_t total,
                    const char *name, size_t *off, size_t *size)
{
    uint16_t i;

    if (h == (const pe_hdr *)0 || buf == (const void *)0 ||
        name == (const char *)0 || off == (size_t *)0 ||
        size == (size_t *)0)
        return PE_ERR_BAD_ARG;
    *off = 0;
    *size = 0;
    /* Division rather than multiplication, so the bound cannot itself
     * overflow on a large NumberOfSections. */
    if (h->sect_table > h->len ||
        (size_t)h->nsect > (h->len - h->sect_table) / PE_SECT_ENTRY)
        return PE_ERR_SHORT;

    for (i = 0; i < h->nsect; i++) {
        pe_sect s;

        /* Inside the window, checked above; tested anyway so no path reads
         * a section header this call did not fill. */
        if (pe_section(h, buf, i, &s) != PE_OK)
            return PE_ERR_SHORT;
        if (!name_is(s.name, name))
            continue;
        /* The first section of that name is the answer, valid or not: a
         * second one behind a malformed first is not something to go looking
         * for in an image nobody built properly. */
        if (s.raw_ptr > total || s.raw_size > total - s.raw_ptr)
            return PE_ERR_RANGE;
        if (s.raw_size == 0)
            return PE_ERR_ABSENT;
        *off = s.raw_ptr;
        *size = s.raw_size;
        return PE_OK;
    }
    return PE_ERR_ABSENT;
}
