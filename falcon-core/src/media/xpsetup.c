/* falcon-core: Windows XP text-mode setup driver injection. See
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
             * layout match this Windows exactly, and a wrong id is "the entry N in [SourceDisksNames] is corrupt
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

/* Puts n bytes at buf + at, moving the rest up. */
static int insert(char *buf, size_t *len, size_t cap, size_t at,
                  const char *bytes, size_t n)
{
    if (*len + n > cap)
        return XPSETUP_ERR_NOSPACE;
    memmove(buf + at + n, buf + at, *len - at);
    memcpy(buf + at, bytes, n);
    *len += n;
    return XPSETUP_OK;
}

int txtsetup_add_hive(char *buf, size_t *len, size_t cap, const char *inf,
                      const char *like)
{
    static const char head[] = "AddReg = ", tail[] = ",AddReg\r\n";
    char line[256];
    size_t i = 0, n = 0, m = 0, o = 0, at = 0;
    int rc;

    if (buf == (char *)0 || len == (size_t *)0 || inf == (const char *)0 ||
        like == (const char *)0)
        return XPSETUP_ERR_BAD_ARG;
    while (inf[n] != '\0')
        n++;
    while (like[m] != '\0')
        m++;

    /* The new [SourceDisksFiles] line, built from `like`'s. */
    while (i < *len) {
        size_t start = i;

        while (i < *len && buf[i] != '\n')
            i++;
        if (i < *len)
            i++;
        if (i - start > m && memcmp(buf + start, like, m) == 0 &&
            (buf[start + m] == ' ' || buf[start + m] == '=') && n + (i - start - m) <
            sizeof line) {
            memcpy(line, inf, n);
            memcpy(line + n, buf + start + m, i - start - m);
            o = n + (i - start - m);
            at = i;
            break;
        }
    }
    if (o == 0)
        return XPSETUP_ERR_NOSECTION;

    /* Found before anything moves, so a missing section changes nothing. */
    i = 0;
    while (i < *len) {
        size_t start = i;

        while (i < *len && buf[i] != '\n')
            i++;
        if (i < *len)
            i++;
        if (!line_is(buf, start, i - start, "[HiveInfs.Fresh]"))
            continue;
        if (*len + o + sizeof head - 1u + n + sizeof tail - 1u > cap)
            return XPSETUP_ERR_NOSPACE;
        rc = insert(buf, len, cap, i, tail, sizeof tail - 1u);
        if (rc == XPSETUP_OK)
            rc = insert(buf, len, cap, i, inf, n);
        if (rc == XPSETUP_OK)
            rc = insert(buf, len, cap, i, head, sizeof head - 1u);
        if (rc == XPSETUP_OK)
            rc = insert(buf, len, cap,
                        at <= start ? at : at + sizeof head - 1u + n +
                                               sizeof tail - 1u,
                        line, o);
        return rc;
    }
    return XPSETUP_ERR_NOSECTION;
}

/* Where a section's first line starts, 0 if the sif has no such section. */
static size_t section_body(const char *buf, size_t len, const char *tag)
{
    size_t i = 0;

    while (i < len) {
        size_t start = i;

        while (i < len && buf[i] != '\n')
            i++;
        if (i < len)
            i++;
        if (line_is(buf, start, i - start, tag))
            return i;
    }
    return 0;
}

/* Whether a line's key is one of the USB drivers a stick needs. */
static int is_usb_storage_row(const char *line, size_t n)
{
    static const char *const keys[] = {
        "usbehci", "usbohci", "usbuhci", "uhcd", "openhci", "usbhub", "usbstor"
    };
    size_t k;

    for (k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        size_t m = 0;

        while (keys[k][m] != '\0')
            m++;
        if (n > m && memcmp(line, keys[k], m) == 0 &&
            (line[m] == ' ' || line[m] == '\t' || line[m] == '='))
            return 1;
    }
    return 0;
}

/* Moves the USB rows of one section to the top of another. */
static int move_usb_rows(char *buf, size_t *len, size_t cap, const char *from,
                         const char *to)
{
    size_t i = section_body(buf, *len, from);

    if (i == 0 || section_body(buf, *len, to) == 0)
        return XPSETUP_ERR_NOSECTION;
    while (i < *len && buf[i] != '[') {
        char line[256];
        size_t start = i, n, at;
        int rc;

        while (i < *len && buf[i] != '\n')
            i++;
        if (i < *len)
            i++;
        n = i - start;
        if (n > sizeof line || !is_usb_storage_row(buf + start, n))
            continue;
        memcpy(line, buf + start, n);
        memmove(buf + start, buf + i, *len - i);
        *len -= n;
        at = section_body(buf, *len, to);
        rc = insert(buf, len, cap, at, line, n);
        if (rc != XPSETUP_OK)
            return rc;
        /* What was at `start` has moved up if the row went in above it. */
        i = at <= start ? start + n : start;
    }
    return XPSETUP_OK;
}

int txtsetup_usb_boot_start(char *buf, size_t *len, size_t cap)
{
    int rc;

    if (buf == (char *)0 || len == (size_t *)0)
        return XPSETUP_ERR_BAD_ARG;
    rc = move_usb_rows(buf, len, cap, "[InputDevicesSupport.Load]",
                       "[BootBusExtenders.Load]");
    if (rc == XPSETUP_OK)
        rc = move_usb_rows(buf, len, cap, "[InputDevicesSupport]",
                           "[BootBusExtenders]");
    return rc;
}

int txtsetup_is_win2000(const char *buf, size_t len)
{
    size_t i = 0;

    if (buf == (const char *)0)
        return 0;
    while (i < len) {
        size_t start = i;

        while (i < len && buf[i] != '\n')
            i++;
        if (i < len)
            i++;
        if (line_is(buf, start, i - start, "MinorVersion = 0"))
            return 1;
    }
    return 0;
}
