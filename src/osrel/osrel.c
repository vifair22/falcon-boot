#include <osrel/osrel.h>

/* Every offset below is checked against len before it is read. The bytes come
 * from inside the LUKS volume the user just unlocked, so this is not hostile
 * input in the way a p2 ISO is - but it is still a length-prefixed structure
 * walk in C, running before ExitBootServices, and "probably friendly" is not a
 * bounds check. */

#define DOS_MAGIC      0x5A4Du      /* "MZ" */
#define PE_LFANEW_OFF  0x3Cu
#define COFF_SECTIONS  6u           /* NumberOfSections, from the PE signature */
#define COFF_OPTSIZE   20u          /* SizeOfOptionalHeader */
#define COFF_SIZE      24u          /* signature + COFF header */
#define SECT_ENTRY     40u
#define SECT_NAME      8u
#define SECT_RAWSIZE   16u
#define SECT_RAWPTR    20u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Section names are 8 bytes, NUL-padded rather than NUL-terminated, so a name
 * that fills the field has no terminator at all. Compare the whole field. */
static int name_is(const uint8_t *field, const char *want)
{
    size_t i;

    for (i = 0; i < SECT_NAME; i++) {
        char w = want[i] != '\0' ? want[i] : '\0';

        if ((char)field[i] != w)
            return 0;
        if (w == '\0' && want[i] == '\0') {
            /* Rest of the field must be padding. */
            for (; i < SECT_NAME; i++) {
                if (field[i] != 0)
                    return 0;
            }
            return 1;
        }
    }
    return 1;
}

/* Finds .osrel and hands back its file range.
 *
 * TWO lengths, and conflating them was a real bug (fixed 2026-09-15). `len` is
 * how much of the file is in b; `total` is how long the file is. The headers
 * and the section table must lie inside b, because they are read here. The
 * SECTION CONTENT must not be required to: a caller that read only a header
 * window is the normal case, and the whole point of returning a file range is
 * that the caller then goes and reads it.
 *
 * Validating the section range against `len` made this function answer "no
 * .osrel section" for every real UKI, whose .osrel sits ~130 KB in, behind a
 * section table at byte 392. The loader fell back to filenames and looked like
 * it simply had not been wired up. */
static int find_osrel(const uint8_t *b, size_t len, size_t total,
                      const char *want, size_t *off, size_t *size)
{
    uint32_t lfanew;
    uint16_t nsect, optsize;
    size_t table;
    size_t i;

    if (len < PE_LFANEW_OFF + 4u)
        return OSREL_ERR_NOT_PE;
    if (rd16(b) != DOS_MAGIC)
        return OSREL_ERR_NOT_PE;

    lfanew = rd32(b + PE_LFANEW_OFF);
    /* lfanew is attacker-controlled in the general case, so it is bounded
     * before it is added to anything. */
    if (lfanew > len || len - lfanew < COFF_SIZE)
        return OSREL_ERR_NOT_PE;
    if (b[lfanew] != 'P' || b[lfanew + 1u] != 'E' ||
        b[lfanew + 2u] != 0 || b[lfanew + 3u] != 0)
        return OSREL_ERR_NOT_PE;

    nsect = rd16(b + lfanew + COFF_SECTIONS);
    optsize = rd16(b + lfanew + COFF_OPTSIZE);
    table = (size_t)lfanew + COFF_SIZE + (size_t)optsize;
    if (table > len)
        return OSREL_ERR_NOT_PE;
    /* Division rather than multiplication, so the bound cannot itself
     * overflow on a large NumberOfSections. */
    if ((size_t)nsect > (len - table) / SECT_ENTRY)
        return OSREL_ERR_NOT_PE;

    for (i = 0; i < (size_t)nsect; i++) {
        const uint8_t *s = b + table + i * SECT_ENTRY;
        uint32_t raw_size, raw_ptr;

        if (!name_is(s, want))
            continue;
        raw_size = rd32(s + SECT_RAWSIZE);
        raw_ptr = rd32(s + SECT_RAWPTR);
        /* Against the FILE, not against the window. */
        if (raw_ptr > total || raw_size > total - raw_ptr)
            return OSREL_ERR_NO_SECTION;
        if (raw_size == 0)
            return OSREL_ERR_NO_SECTION;
        *off = raw_ptr;
        *size = raw_size;
        return OSREL_OK;
    }
    return OSREL_ERR_NO_SECTION;
}

static int key_matches(const char *text, size_t len, size_t at, const char *key)
{
    size_t i = 0;

    while (key[i] != '\0') {
        if (at + i >= len || text[at + i] != key[i])
            return 0;
        i++;
    }
    return at + i < len && text[at + i] == '=';
}

int osrel_find(const void *buf, size_t len, size_t total,
               const char *section, size_t *off, size_t *size)
{
    size_t o = 0, n = 0;
    int rc;

    if (off == (size_t *)0 || size == (size_t *)0 || section == (const char *)0)
        return OSREL_ERR_BAD_ARG;
    *off = 0;
    *size = 0;
    if (buf == (const void *)0 || total < len)
        return OSREL_ERR_BAD_ARG;
    rc = find_osrel((const uint8_t *)buf, len, total, section, &o, &n);
    if (rc != OSREL_OK)
        return rc;
    *off = o;
    *size = n;
    return OSREL_OK;
}

int osrel_value(const char *text, size_t len, const char *key,
                char *out, size_t cap)
{
    size_t i = 0;

    if (out == (char *)0 || cap == 0)
        return OSREL_ERR_BAD_ARG;
    out[0] = '\0';
    if (text == (const char *)0 || key == (const char *)0 || len == 0)
        return OSREL_ERR_BAD_ARG;

    while (i < len) {
        size_t j;
        size_t n = 0;
        char quote = '\0';

        /* Start of a line. Leading blanks are tolerated; a comment is not a
         * key even if it spells one. */
        while (i < len && (text[i] == ' ' || text[i] == '\t'))
            i++;
        if (i < len && text[i] == '#') {
            while (i < len && text[i] != '\n')
                i++;
            i++;
            continue;
        }
        if (!key_matches(text, len, i, key)) {
            while (i < len && text[i] != '\n')
                i++;
            i++;
            continue;
        }

        j = i;
        while (key[n] != '\0')
            n++;
        j += n + 1u;                     /* past KEY= */
        if (j < len && (text[j] == '"' || text[j] == '\'')) {
            quote = text[j];
            j++;
        }
        n = 0;
        while (j < len && text[j] != '\n') {
            char c = text[j];

            if (quote != '\0' && c == quote)
                break;
            if (quote == '\0' && (c == ' ' || c == '\t'))
                break;
            /* Printable ASCII only. A control byte in a label would be drawn
             * straight to a console that treats some of them as commands.
             *
             * The buffer is emptied before returning, not just left as it is:
             * by here it already holds the accepted prefix, and handing that
             * back with an error would put a half-parsed name in front of a
             * caller that ignored the return value. That is the exact outcome
             * the check exists to prevent, so it must not be the way the check
             * fails. */
            if (c < 0x20 || c > 0x7E) {
                out[0] = '\0';
                return OSREL_ERR_NO_NAME;
            }
            if (n + 1u >= cap)
                break;                   /* truncate, do not refuse */
            out[n++] = c;
            j++;
        }
        out[n] = '\0';
        return n != 0 ? OSREL_OK : OSREL_ERR_NO_NAME;
    }
    return OSREL_ERR_NO_NAME;
}

int osrel_label(const void *buf, size_t len, char *out, size_t cap)
{
    const uint8_t *b = (const uint8_t *)buf;
    const char *text;
    size_t off = 0, size = 0;
    size_t n = 0;
    char ver[OSREL_LABEL_MAX];
    int rc;

    if (out == (char *)0 || cap == 0)
        return OSREL_ERR_BAD_ARG;
    out[0] = '\0';
    if (b == (const uint8_t *)0)
        return OSREL_ERR_BAD_ARG;

    /* Whole-buffer caller: the window IS the file. */
    rc = find_osrel(b, len, len, OSREL_SECT_OSREL, &off, &size);
    if (rc != OSREL_OK)
        return rc;
    text = (const char *)(b + off);

    /* systemd-boot's order, so an image labelled one way there is labelled the
     * same way here. */
    if (osrel_value(text, size, "PRETTY_NAME", out, cap) == OSREL_OK)
        return OSREL_OK;
    if (osrel_value(text, size, "NAME", out, cap) != OSREL_OK) {
        out[0] = '\0';
        return OSREL_ERR_NO_NAME;
    }

    /* NAME alone is often just "Arch Linux" for every image on the shelf, so
     * VERSION_ID earns its place when there is one - but only when NAME does
     * not already carry it, which is the common Fedora shape. */
    if (osrel_value(text, size, "VERSION_ID", ver, sizeof ver) == OSREL_OK) {
        size_t i;

        while (out[n] != '\0')
            n++;
        for (i = 0; ver[i] != '\0'; i++) {
            /* Already present? Then adding it again is noise. */
            size_t k = 0;

            while (out[k] != '\0') {
                size_t m = 0;

                while (ver[m] != '\0' && out[k + m] == ver[m])
                    m++;
                if (ver[m] == '\0')
                    return OSREL_OK;
                k++;
            }
            break;
        }
        if (ver[0] != '\0' && n + 2u < cap) {
            out[n++] = ' ';
            for (i = 0; ver[i] != '\0' && n + 1u < cap; i++)
                out[n++] = ver[i];
            out[n] = '\0';
        }
    }
    return OSREL_OK;
}
