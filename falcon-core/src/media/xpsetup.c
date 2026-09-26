/* falcon-core: Windows 2000/XP text-mode setup driver injection. See
 * xpsetup.h. */

#include <media/xpsetup.h>

#include <lib/string.h>

/* Appends a NUL-terminated string to out at *o, bounded by cap. */
static int put_str(char *out, size_t *o, size_t cap, const char *s)
{
    while (*s != '\0') {
        if (*o >= cap)
            return -1;
        out[(*o)++] = *s++;
    }
    return 0;
}

/* A line's content (in[start..start+len]) equals `tag` and nothing but its
 * CR/LF follows. */
static int line_is(const char *in, size_t start, size_t len, const char *tag)
{
    size_t n = 0;

    while (tag[n] != '\0') {
        if (n >= len || in[start + n] != tag[n])
            return 0;
        n++;
    }
    /* the rest of the line must be only the CR/LF terminator */
    while (n < len) {
        char c = in[start + n];

        if (c != '\r' && c != '\n')
            return 0;
        n++;
    }
    return 1;
}

int txtsetup_inject(const char *in, size_t in_len,
                    const char *svc, const char *file, const char *desc,
                    char *out, size_t out_cap, size_t *out_len)
{
    size_t i = 0, o = 0;
    int did_load = 0, did_scsi = 0, did_src = 0;
    int in_src = 0;               /* inside [SourceDisksFiles], before its first
                                   * entry, which the driver's line copies */

    if (in == (const char *)0 || svc == (const char *)0 ||
        file == (const char *)0 || desc == (const char *)0 ||
        out == (char *)0 || out_len == (size_t *)0)
        return XPSETUP_ERR_BAD_ARG;

    while (i < in_len) {
        size_t start = i;
        size_t linelen;

        while (i < in_len && in[i] != '\n')
            i++;
        if (i < in_len)
            i++;                          /* take the '\n' too */
        linelen = i - start;

        if (o + linelen > out_cap)
            return XPSETUP_ERR_NOSPACE;
        memcpy(out + o, in + start, linelen);
        o += linelen;

        /* Insert the driver's line right after its section header, once. The
         * lines mirror atapi.sys's own so setup treats it the same way. */
        if (!did_load && line_is(in, start, linelen, "[SCSI.Load]")) {
            if (put_str(out, &o, out_cap, svc) != 0 ||
                put_str(out, &o, out_cap, " = ") != 0 ||
                put_str(out, &o, out_cap, file) != 0 ||
                put_str(out, &o, out_cap, ",4\r\n") != 0)
                return XPSETUP_ERR_NOSPACE;
            did_load = 1;
        } else if (!did_scsi && line_is(in, start, linelen, "[SCSI]")) {
            if (put_str(out, &o, out_cap, svc) != 0 ||
                put_str(out, &o, out_cap, " = \"") != 0 ||
                put_str(out, &o, out_cap, desc) != 0 ||
                put_str(out, &o, out_cap, "\"\r\n") != 0)
                return XPSETUP_ERR_NOSPACE;
            did_scsi = 1;
        } else if (!did_src &&
                   line_is(in, start, linelen, "[SourceDisksFiles]")) {
            in_src = 1;
        } else if (in_src) {
            /* Copy the driver's SourceDisksFiles value from the section's first
             * real entry (e.g. atapi.sys), so the source-disk id and field
             * layout match this Windows exactly: XP names it 100, 2000 names it
             * 1, and a wrong id is "the entry N in [SourceDisksNames] is corrupt
             * or missing". Blank and comment lines have no '=' and are skipped. */
            size_t eq = start;

            while (eq < start + linelen && in[eq] != '=' &&
                   in[eq] != '\r' && in[eq] != '\n')
                eq++;
            if (eq < start + linelen && in[eq] == '=') {
                size_t v = eq + 1, ve;

                while (v < start + linelen && (in[v] == ' ' || in[v] == '\t'))
                    v++;
                ve = v;
                while (ve < start + linelen && in[ve] != '\r' && in[ve] != '\n')
                    ve++;
                if (put_str(out, &o, out_cap, file) != 0 ||
                    put_str(out, &o, out_cap, " = ") != 0)
                    return XPSETUP_ERR_NOSPACE;
                if (o + (ve - v) + 2u > out_cap)
                    return XPSETUP_ERR_NOSPACE;
                memcpy(out + o, in + v, ve - v);
                o += ve - v;
                out[o++] = '\r';
                out[o++] = '\n';
                in_src = 0;
                did_src = 1;
            }
        }
    }

    if (!did_load || !did_scsi || !did_src)
        return XPSETUP_ERR_NOSECTION;
    *out_len = o;
    return XPSETUP_OK;
}
