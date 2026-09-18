#include <media/osrel.h>

#include <pe/pe.h>

/* Every offset below is checked against len before it is read; the PE walk
 * itself is falcon-core/src/pe's. The bytes come from inside the LUKS volume
 * the user just unlocked, so this is not hostile input in the way a p2 ISO is -
 * but it is still a length-prefixed structure walk in C, running before
 * ExitBootServices, and "probably friendly" is not a bounds check. */

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

/* The PE walk is falcon-core/src/pe's. What stays here is the mapping onto this
 * module's codes, which predate it and which pos.c reports: anything about the
 * headers or a section table past the window is "not a PE we recognise", and a
 * section that is missing, empty or runs past the end of the file is "no
 * section". */
int osrel_find(const void *buf, size_t len, size_t total,
               const char *section, size_t *off, size_t *size)
{
    pe_hdr h;
    int rc;

    if (off == (size_t *)0 || size == (size_t *)0 || section == (const char *)0)
        return OSREL_ERR_BAD_ARG;
    *off = 0;
    *size = 0;
    if (buf == (const void *)0 || total < len)
        return OSREL_ERR_BAD_ARG;
    if (pe_parse(buf, len, &h) != PE_OK)
        return OSREL_ERR_NOT_PE;
    rc = pe_find_section(&h, buf, total, section, off, size);
    if (rc == PE_OK)
        return OSREL_OK;
    return rc == PE_ERR_SHORT ? OSREL_ERR_NOT_PE : OSREL_ERR_NO_SECTION;
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
    rc = osrel_find(b, len, len, OSREL_SECT_OSREL, &off, &size);
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

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

int osrel_vercmp(const char *a, const char *b)
{
    if (a == (const char *)0 || b == (const char *)0)
        return (a != (const char *)0) - (b != (const char *)0);

    while (*a != '\0' && *b != '\0') {
        if (is_digit(*a) && is_digit(*b)) {
            const char *ea, *eb;
            size_t la, lb, i;

            /* Leading zeros carry no value; after them, the longer run of
             * digits is the larger number, and equal lengths compare as
             * strings. No conversion, so no run is too long to compare. */
            while (*a == '0')
                a++;
            while (*b == '0')
                b++;
            for (ea = a; is_digit(*ea); ea++)
                ;
            for (eb = b; is_digit(*eb); eb++)
                ;
            la = (size_t)(ea - a);
            lb = (size_t)(eb - b);
            if (la != lb)
                return la < lb ? -1 : 1;
            for (i = 0; i < la; i++) {
                if (a[i] != b[i])
                    return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
            }
            a = ea;
            b = eb;
            continue;
        }
        if (*a != *b)
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        a++;
        b++;
    }
    return (*a != '\0') - (*b != '\0');
}
