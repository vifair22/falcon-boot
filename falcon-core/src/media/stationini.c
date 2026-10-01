/* falcon-core: STATION.INI. See stationini.h.
 *
 * Written in jumpini.c's shape: one pass over the lines, every error a
 * sentence naming the line, no libc beyond the mem* routines, the sentence
 * built by hand. It runs in both loaders and in the installer, so it owns no
 * storage: everything it keeps is in the caller's stationini.
 *
 * A default has to name something, but the entries it can name are not all
 * in the file: the loader finds some itself and names them by rule
 * (DESIGN Section 12.7, "Autodetected entry names").
 * So a default that names no section is accepted only when it has the shape
 * of such a name; whether that entry exists is the loader's to find out on
 * the machine, since the parser cannot. Anything else naming no section is
 * the typo the strictness is for.
 */

#include <media/stationini.h>

#include <lib/string.h>

#define GENERAL_LINE_NONE 0u

static unsigned char lower(char c)
{
    unsigned char u = (unsigned char)c;

    return (u >= 'A' && u <= 'Z') ? (unsigned char)(u + ('a' - 'A')) : u;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) ||
           c == '_' || c == '-';
}

/* [a, b) against a NUL-terminated lower-case word, ignoring case. */
static int word_eq(const char *a, const char *b, const char *word)
{
    while (a < b && *word != '\0') {
        if (lower(*a) != (unsigned char)*word)
            return 0;
        a++;
        word++;
    }
    return a == b && *word == '\0';
}

/* [a, b) against a NUL-terminated string, exactly. */
static int span_eq(const char *a, const char *b, const char *s)
{
    while (a < b && *s != '\0') {
        if (*a != *s)
            return 0;
        a++;
        s++;
    }
    return a == b && *s == '\0';
}

static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static size_t str_len(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

typedef struct {
    char  *at;
    size_t left;
} sbuf;

static void put(sbuf *s, const char *str)
{
    while (*str != '\0' && s->left > 1u) {
        *s->at++ = *str++;
        s->left--;
    }
    *s->at = '\0';
}

/* [a, b), never past 24 characters, so a line of garbage cannot push the
 * rest of the sentence off the screen. */
static void put_span(sbuf *s, const char *a, const char *b)
{
    size_t n = 0;

    while (a < b && s->left > 1u && n < 24u) {
        *s->at++ = *a++;
        s->left--;
        n++;
    }
    *s->at = '\0';
}

static int fail(stationini *out, unsigned line, const char *what,
                const char *a, const char *b, const char *after)
{
    sbuf s;

    out->err_line = line;
    s.at = out->why;
    s.left = sizeof out->why;
    put(&s, what);
    if (a != (const char *)0) {
        put(&s, " '");
        put_span(&s, a, b);
        put(&s, "'");
    }
    put(&s, after);
    /* Last: the span may be the default itself. */
    out->default_name[0] = '\0';
    out->timeout = 0;
    out->poweroff = 0;
    out->poweroff_set = 0;
    out->totp.present = 0;
    return -1;
}

static int fail_plain(stationini *out, unsigned line, const char *what)
{
    return fail(out, line, what, (const char *)0, (const char *)0, "");
}

/* A name: [A-Za-z0-9_-], 1 to STATIONINI_NAME_MAX-1 characters. */
static int name_ok(const char *a, const char *b)
{
    const char *p;

    if (a == b || (size_t)(b - a) >= STATIONINI_NAME_MAX)
        return 0;
    for (p = a; p < b; p++)
        if (!is_name_char(*p))
            return 0;
    return 1;
}

static void copy_span(char *dst, const char *a, const char *b)
{
    size_t n = (size_t)(b - a);

    memcpy(dst, a, n);
    dst[n] = '\0';
}

/* Exactly n hex digits at [a, a+n), inside [a, b). */
static int hex_run(const char *a, const char *b, size_t n)
{
    size_t i;

    if ((size_t)(b - a) < n)
        return 0;
    for (i = 0; i < n; i++)
        if (!is_hex(a[i]))
            return 0;
    return 1;
}

/* "<8 hex>" or "<8 hex>-p<digits>" (a disk's MBR, or one of its
 * partitions). */
static int disk_tail_ok(const char *a, const char *b)
{
    if (!hex_run(a, b, 8))
        return 0;
    a += 8;
    if (a == b)
        return 1;
    if (b - a < 3 || a[0] != '-' || a[1] != 'p')
        return 0;
    for (a += 2; a < b; a++)
        if (!is_digit(*a))
            return 0;
    return 1;
}

/* The loader's generated names (DESIGN Section 12.7). [a, b) is
 * already a name. */
static int autoname_ok(const char *a, const char *b)
{
    if (span_eq(a, b, "memtest") || span_eq(a, b, "pos"))
        return 1;
    if (b - a < 5)
        return 0;
    if (span_eq(a, a + 4, "mbr-") || span_eq(a, a + 4, "gpt-"))
        return disk_tail_ok(a + 4, b);
    if (span_eq(a, a + 4, "nxt-"))
        return b - a == 8 && hex_run(a + 4, b, 4);
    return span_eq(a, a + 4, "uki-");
}

/* A value kept as text (label, uki, efi): printable ASCII, and shorter than
 * its field. The line is already trimmed, so it cannot start or end in a
 * space, and a comment character never reaches here. */
static int text_ok(const char *a, const char *b, size_t cap)
{
    const char *p;

    if (a == b || (size_t)(b - a) >= cap)
        return 0;
    for (p = a; p < b; p++)
        if ((unsigned char)*p < 0x20u || (unsigned char)*p > 0x7Eu)
            return 0;
    return 1;
}

static int parse_uint(const char *a, const char *b, uint32_t max,
                      uint32_t *v)
{
    uint32_t n = 0;

    if (a == b)
        return -1;
    for (; a < b; a++) {
        if (!is_digit(*a))
            return -1;
        n = n * 10u + (uint32_t)(*a - '0');
        if (n > max)
            return -1;
    }
    *v = n;
    return 0;
}

static int parse_tri(const char *a, const char *b, stationini_tri *t)
{
    if (word_eq(a, b, "yes")) {
        *t = STATIONINI_TRI_YES;
        return 0;
    }
    if (word_eq(a, b, "no")) {
        *t = STATIONINI_TRI_NO;
        return 0;
    }
    return -1;
}

/* order = a, b, c: names, each once. */
static int parse_order(stationini *out, unsigned line, const char *a,
                       const char *b)
{
    if (a == b)
        return fail_plain(out, line, "order names nothing");
    for (;;) {
        const char *ia = a;
        const char *ib;
        const char *next;
        unsigned i;

        while (a < b && *a != ',')
            a++;
        next = a;
        ib = a;
        while (ia < ib && is_space(*ia))
            ia++;
        while (ib > ia && is_space(ib[-1]))
            ib--;
        if (!name_ok(ia, ib))
            return fail(out, line, "order entry", ia, ib, " is not a name");
        for (i = 0; i < out->n_order; i++)
            if (span_eq(ia, ib, out->order[i]))
                return fail(out, line, "order names", ia, ib, " twice");
        if (out->n_order >= STATIONINI_ORDER_MAX)
            return fail_plain(out, line, "order names more than 32 entries");
        copy_span(out->order[out->n_order++], ia, ib);
        if (next == b)
            return 0;
        a = next + 1;
    }
}

enum {
    SEEN_DISK   = 1u << 0,
    SEEN_PART   = 1u << 1,
    SEEN_UKI    = 1u << 2,
    SEEN_EFI    = 1u << 3,
    SEEN_METHOD = 1u << 4,
    SEEN_SWAP   = 1u << 5,
    SEEN_LABEL  = 1u << 6,
    SEEN_HIDE   = 1u << 7,
    SEEN_DEF    = 1u << 8,
    SEEN_TMO    = 1u << 9,
    SEEN_ORDER  = 1u << 10,
    SEEN_TPM    = 1u << 11,
    SEEN_INDEX  = 1u << 12,
    SEEN_PCRS   = 1u << 13,
    SEEN_KEY    = 1u << 14,
    SEEN_TLABEL = 1u << 15,
    SEEN_SRKAUTH = 1u << 16,
    SEEN_POWEROFF = 1u << 17
};

typedef struct {
    const char *word;
    unsigned    bit;
} key_def;

static const key_def k_entry_keys[] = {
    { "disk", SEEN_DISK }, { "part", SEEN_PART }, { "uki", SEEN_UKI },
    { "efi", SEEN_EFI }, { "method", SEEN_METHOD }, { "swap", SEEN_SWAP },
    { "label", SEEN_LABEL }, { "hide", SEEN_HIDE }
};

static const key_def k_general_keys[] = {
    { "default", SEEN_DEF }, { "timeout", SEEN_TMO }, { "order", SEEN_ORDER },
    { "poweroff", SEEN_POWEROFF }
};

static const key_def k_totp_keys[] = {
    { "tpm", SEEN_TPM }, { "index", SEEN_INDEX }, { "pcrs", SEEN_PCRS },
    { "key", SEEN_KEY }, { "label", SEEN_TLABEL },
    { "srkauth", SEEN_SRKAUTH }
};

#define TOTP_REQUIRED (SEEN_TPM | SEEN_INDEX | SEEN_PCRS | SEEN_KEY)

static unsigned key_bit(const key_def *keys, size_t n, const char *a,
                        const char *b, const char **word)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (word_eq(a, b, keys[i].word)) {
            *word = keys[i].word;
            return keys[i].bit;
        }
    }
    return 0;
}

static int entry_value(stationini *out, stationini_entry *e, unsigned line,
                       unsigned bit, const char *va, const char *vb)
{
    uint32_t n;

    switch (bit) {
    case SEEN_DISK:
        if (part_disk_id_parse(va, (size_t)(vb - va), &e->disk) != PART_OK)
            return fail(out, line, "disk", va, vb,
                        " is not mbr:<8 hex> or gpt:<GUID>");
        e->has_disk = 1;
        return 0;
    case SEEN_PART:
        if (parse_uint(va, vb, 9999u, &n) != 0 || n == 0)
            return fail(out, line, "part", va, vb,
                        " is not a partition number");
        e->part = n;
        return 0;
    case SEEN_UKI:
    case SEEN_EFI:
        if (!text_ok(va, vb, STATIONINI_PATH_MAX))
            return fail(out, line, bit == SEEN_UKI ? "uki" : "efi", va, vb,
                        " is not a path of up to 127 characters");
        copy_span(bit == SEEN_UKI ? e->uki : e->efi, va, vb);
        return 0;
    case SEEN_METHOD:
        if (word_eq(va, vb, "firmware"))
            e->method = STATIONINI_METHOD_FIRMWARE;
        else if (word_eq(va, vb, "efi"))
            e->method = STATIONINI_METHOD_EFI;
        else
            return fail(out, line, "method", va, vb,
                        " is not firmware or efi");
        return 0;
    case SEEN_SWAP:
        if (parse_tri(va, vb, &e->swap) != 0)
            return fail(out, line, "swap", va, vb, " is not yes or no");
        return 0;
    case SEEN_LABEL:
        if (!text_ok(va, vb, STATIONINI_LABEL_MAX))
            return fail(out, line, "label", va, vb,
                        " is not text of up to 63 characters");
        copy_span(e->label, va, vb);
        return 0;
    default: /* SEEN_HIDE */
        if (parse_tri(va, vb, &e->hide) != 0)
            return fail(out, line, "hide", va, vb, " is not yes or no");
        return 0;
    }
}

static unsigned hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

/* pcrs = 0, 2, 4, 7: PCR numbers below STATIONINI_TOTP_PCRS, each once. */
static int parse_pcrs(const char *a, const char *b, uint32_t *mask)
{
    uint32_t m = 0;

    for (;;) {
        const char *ia = a;
        const char *ib;
        uint32_t n;

        while (a < b && *a != ',')
            a++;
        ib = a;
        while (ia < ib && is_space(*ia))
            ia++;
        while (ib > ia && is_space(ib[-1]))
            ib--;
        if (parse_uint(ia, ib, STATIONINI_TOTP_PCRS - 1u, &n) != 0 ||
            (m & (1u << n)) != 0)
            return -1;
        m |= 1u << n;
        if (a == b)
            break;
        a++;
    }
    *mask = m;
    return 0;
}

static int totp_value(stationini *out, unsigned line, unsigned bit,
                      const char *va, const char *vb)
{
    stationini_totp *t = &out->totp;
    size_t n = (size_t)(vb - va);
    size_t i;

    switch (bit) {
    case SEEN_TPM:
        if (span_eq(va, vb, "2.0"))
            t->tpm = 20;
        else if (span_eq(va, vb, "1.2"))
            t->tpm = 12;
        else
            return fail(out, line, "tpm", va, vb, " is not 2.0 or 1.2");
        return 0;
    case SEEN_INDEX:
        if (n < 3 || n > 10 || va[0] != '0' || lower(va[1]) != 'x' ||
            !hex_run(va + 2, vb, n - 2u))
            return fail(out, line, "index", va, vb,
                        " is not 0x and up to 8 hex digits");
        t->index = 0;
        for (i = 2; i < n; i++)
            t->index = (t->index << 4) | hex_val(va[i]);
        return 0;
    case SEEN_PCRS:
        if (parse_pcrs(va, vb, &t->pcrs) != 0)
            return fail(out, line, "pcrs", va, vb,
                        " is not PCR numbers 0 to 15, each once");
        return 0;
    case SEEN_KEY:
        if (n == 0 || (n & 1u) != 0 || n > (size_t)2 * STATIONINI_TOTP_KEY_MAX ||
            !hex_run(va, vb, n))
            return fail(out, line, "key", va, vb,
                        " is not hex of up to 256 bytes");
        for (i = 0; i < n; i += 2)
            t->key[i / 2u] = (uint8_t)((hex_val(va[i]) << 4) |
                                       hex_val(va[i + 1u]));
        t->key_len = (uint16_t)(n / 2u);
        return 0;
    case SEEN_SRKAUTH:
        if (word_eq(va, vb, "zero"))
            t->srk_sha1 = 0;
        else if (word_eq(va, vb, "sha1-zero"))
            t->srk_sha1 = 1;
        else
            return fail(out, line, "srkauth", va, vb,
                        " is not zero or sha1-zero");
        return 0;
    default: /* SEEN_TLABEL */
        if (!text_ok(va, vb, STATIONINI_LABEL_MAX))
            return fail(out, line, "label", va, vb,
                        " is not text of up to 63 characters");
        copy_span(t->label, va, vb);
        return 0;
    }
}

/* A key the version can use: a 2.0 SRK's name is its nameAlg (SHA-256,
 * 0x000B) and the 32-byte digest; a 1.2 SRK's modulus is 1024 to 2048
 * bits. */
static int totp_key_ok(const stationini_totp *t)
{
    if (t->tpm == 20)
        return t->key_len == 34u && t->key[0] == 0x00u &&
               t->key[1] == 0x0Bu;
    return t->key_len >= 128u && t->key_len <= STATIONINI_TOTP_KEY_MAX;
}

static int totp_close(stationini *out, unsigned line, unsigned seen)
{
    if ((seen & TOTP_REQUIRED) != TOTP_REQUIRED)
        return fail_plain(out, line,
                          "[totp] needs tpm, index, pcrs and key");
    if (!totp_key_ok(&out->totp))
        return fail_plain(out, line,
                          "[totp] key is not a key that TPM version has");
    if (out->totp.srk_sha1 && out->totp.tpm != 12)
        return fail_plain(out, line, "[totp] srkauth is for a TPM 1.2");
    out->totp.present = 1;
    return 0;
}

/* What a finished section must hold. */
static int entry_close(stationini *out, const stationini_entry *e)
{
    if (e != (const stationini_entry *)0 && e->part != 0 && !e->has_disk)
        return fail_plain(out, e->line, "part is given without disk");
    return 0;
}

int stationini_parse(const char *text, size_t len, stationini *out)
{
    const char *p = text;
    const char *end = text + len;
    unsigned line = 0;
    unsigned seen = 0;
    int in_general = 0;
    int in_totp = 0;
    unsigned general_line = GENERAL_LINE_NONE;
    unsigned totp_line = 0;
    unsigned default_line = 0;
    unsigned timeout_line = 0;
    stationini_entry *cur = (stationini_entry *)0;

    if (out == (stationini *)0)
        return -1;
    memset(out, 0, sizeof *out);
    if (text == (const char *)0)
        return len == 0 ? 0 : -1;

    while (p < end) {
        const char *ls = p;
        const char *le;
        const char *eq;
        const char *ka;
        const char *kb;
        const char *va;
        const char *vb;
        const char *word = "";
        unsigned bit;

        line++;
        while (p < end && *p != '\n')
            p++;
        le = p;
        if (p < end)
            p++;

        for (eq = ls; eq < le; eq++) {
            if (*eq == ';' || *eq == '#') {
                le = eq;
                break;
            }
        }
        while (ls < le && is_space(*ls))
            ls++;
        while (le > ls && is_space(le[-1]))
            le--;
        if (ls == le)
            continue;

        if (*ls == '[') {
            const char *na = ls + 1;
            const char *nb = le - 1;
            unsigned i;

            if (*nb != ']')
                return fail(out, line, "a section header that is not [name]",
                            ls, le, "");
            while (na < nb && is_space(*na))
                na++;
            while (nb > na && is_space(nb[-1]))
                nb--;
            if (entry_close(out, cur) != 0)
                return -1;
            if (in_totp && totp_close(out, totp_line, seen) != 0)
                return -1;
            cur = (stationini_entry *)0;
            seen = 0;
            in_totp = 0;
            if (word_eq(na, nb, "totp")) {
                if (totp_line != 0)
                    return fail_plain(out, line,
                                      "section [totp] is given twice");
                totp_line = line;
                in_totp = 1;
                in_general = 0;
                continue;
            }
            if (word_eq(na, nb, "general")) {
                if (general_line != GENERAL_LINE_NONE)
                    return fail_plain(out, line,
                                      "section [general] is given twice");
                general_line = line;
                in_general = 1;
                continue;
            }
            in_general = 0;
            if (!name_ok(na, nb))
                return fail(out, line, "section", na, nb,
                            " is not a name ([A-Za-z0-9_-], up to 31)");
            for (i = 0; i < out->n_entries; i++)
                if (span_eq(na, nb, out->entries[i].name))
                    return fail(out, line, "section", na, nb,
                                " is given twice");
            if (out->n_entries >= STATIONINI_ENTRIES_MAX)
                return fail_plain(out, line, "more than 32 entry sections");
            cur = &out->entries[out->n_entries++];
            copy_span(cur->name, na, nb);
            cur->line = line;
            continue;
        }

        for (eq = ls; eq < le && *eq != '='; eq++)
            ;
        if (eq == le)
            return fail(out, line, "a line that is not key=value", ls, le, "");
        ka = ls;
        kb = eq;
        while (kb > ka && is_space(kb[-1]))
            kb--;
        va = eq + 1;
        vb = le;
        while (va < vb && is_space(*va))
            va++;

        if (in_general)
            bit = key_bit(k_general_keys,
                          sizeof k_general_keys / sizeof k_general_keys[0],
                          ka, kb, &word);
        else if (in_totp)
            bit = key_bit(k_totp_keys,
                          sizeof k_totp_keys / sizeof k_totp_keys[0],
                          ka, kb, &word);
        else if (cur != (stationini_entry *)0)
            bit = key_bit(k_entry_keys,
                          sizeof k_entry_keys / sizeof k_entry_keys[0],
                          ka, kb, &word);
        else
            return fail(out, line, "key", ka, kb, " is outside a section");
        if (bit == 0)
            return fail(out, line, "unknown key", ka, kb, "");
        if ((seen & bit) != 0)
            return fail(out, line, word, (const char *)0, (const char *)0,
                        " is given twice");
        seen |= bit;

        if (bit == SEEN_DEF) {
            if (!name_ok(va, vb))
                return fail(out, line, "default", va, vb, " is not a name");
            copy_span(out->default_name, va, vb);
            default_line = line;
        } else if (bit == SEEN_TMO) {
            uint32_t n;

            if (parse_uint(va, vb, STATIONINI_TIMEOUT_MAX, &n) != 0)
                return fail(out, line, "timeout", va, vb,
                            " is not a number of seconds up to 3600");
            out->timeout = (unsigned)n;
            timeout_line = line;
        } else if (bit == SEEN_POWEROFF) {
            uint32_t n;

            if (parse_uint(va, vb, STATIONINI_POWEROFF_MAX, &n) != 0)
                return fail(out, line, "poweroff", va, vb,
                            " is not a number of seconds up to 86400");
            out->poweroff = (unsigned)n;
            out->poweroff_set = 1;
        } else if (bit == SEEN_ORDER) {
            if (parse_order(out, line, va, vb) != 0)
                return -1;
        } else if (in_totp) {
            if (totp_value(out, line, bit, va, vb) != 0)
                return -1;
        } else if (entry_value(out, cur, line, bit, va, vb) != 0) {
            return -1;
        }
    }

    if (entry_close(out, cur) != 0)
        return -1;
    if (in_totp && totp_close(out, totp_line, seen) != 0)
        return -1;
    if (out->timeout != 0 && out->default_name[0] == '\0')
        return fail_plain(out, timeout_line,
                          "timeout is set but default is not");
    if (out->default_name[0] != '\0' &&
        stationini_find(out, out->default_name) == (stationini_entry *)0 &&
        !autoname_ok(out->default_name,
                     out->default_name + str_len(out->default_name)))
        return fail(out, default_line, "default",
                    out->default_name,
                    out->default_name + str_len(out->default_name),
                    " names no entry");
    return 0;
}

const stationini_entry *stationini_find(const stationini *c, const char *name)
{
    unsigned i;

    if (c == (const stationini *)0 || name == (const char *)0)
        return (const stationini_entry *)0;
    for (i = 0; i < c->n_entries && i < STATIONINI_ENTRIES_MAX; i++)
        if (str_eq(c->entries[i].name, name))
            return &c->entries[i];
    return (const stationini_entry *)0;
}

/* ------------------------------------------------------------- the writer */

typedef struct {
    char  *at;
    size_t left;   /* bytes left, the terminator's included */
    int    over;
} wbuf;

static void w_str(wbuf *w, const char *s)
{
    while (*s != '\0') {
        if (w->left <= 1u) {
            w->over = 1;
            return;
        }
        *w->at++ = *s++;
        w->left--;
    }
}

static void w_kv(wbuf *w, const char *key, const char *val)
{
    w_str(w, key);
    w_str(w, " = ");
    w_str(w, val);
    w_str(w, "\n");
}

static void w_uint(wbuf *w, uint32_t v)
{
    char tmp[11];
    char *p = tmp + sizeof tmp - 1u;

    *p = '\0';
    do {
        *--p = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    w_str(w, p);
}

static int str_name_ok(const char *s, size_t cap)
{
    size_t n = 0;

    while (n < cap && s[n] != '\0')
        n++;
    return n < cap && name_ok(s, s + n);
}

/* A text field the parser reads back unchanged: printable, no comment
 * character, no space at either end. Empty means absent. */
static int str_text_ok(const char *s, size_t cap)
{
    size_t n = 0;
    size_t i;

    while (n < cap && s[n] != '\0')
        n++;
    if (n == cap)
        return 0;
    if (n == 0)
        return 1;
    if (!text_ok(s, s + n, cap) || s[0] == ' ' || s[n - 1u] == ' ')
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] == ';' || s[i] == '#')
            return 0;
    return 1;
}

static int tri_ok(stationini_tri t)
{
    return t == STATIONINI_TRI_UNSET || t == STATIONINI_TRI_NO ||
           t == STATIONINI_TRI_YES;
}

/* Everything the parser would refuse, refused before a byte is written. */
static int format_ok(const stationini *c)
{
    unsigned i;
    unsigned j;

    if (c->n_entries > STATIONINI_ENTRIES_MAX ||
        c->n_order > STATIONINI_ORDER_MAX ||
        c->timeout > STATIONINI_TIMEOUT_MAX ||
        (c->poweroff_set && c->poweroff > STATIONINI_POWEROFF_MAX))
        return 0;
    if (c->default_name[0] != '\0') {
        size_t n;

        if (!str_name_ok(c->default_name, sizeof c->default_name))
            return 0;
        n = str_len(c->default_name);
        if (stationini_find(c, c->default_name) == (stationini_entry *)0 &&
            !autoname_ok(c->default_name, c->default_name + n))
            return 0;
    } else if (c->timeout != 0) {
        return 0;
    }
    for (i = 0; i < c->n_order; i++) {
        if (!str_name_ok(c->order[i], sizeof c->order[i]))
            return 0;
        for (j = 0; j < i; j++)
            if (str_eq(c->order[i], c->order[j]))
                return 0;
    }
    for (i = 0; i < c->n_entries; i++) {
        const stationini_entry *e = &c->entries[i];

        if (!str_name_ok(e->name, sizeof e->name) ||
            word_eq(e->name, e->name + str_len(e->name), "general") ||
            word_eq(e->name, e->name + str_len(e->name), "totp"))
            return 0;
        for (j = 0; j < i; j++)
            if (str_eq(e->name, c->entries[j].name))
                return 0;
        if (!str_text_ok(e->uki, sizeof e->uki) ||
            !str_text_ok(e->efi, sizeof e->efi) ||
            !str_text_ok(e->label, sizeof e->label) ||
            !tri_ok(e->swap) || !tri_ok(e->hide) ||
            (e->method != STATIONINI_METHOD_UNSET &&
             e->method != STATIONINI_METHOD_FIRMWARE &&
             e->method != STATIONINI_METHOD_EFI) ||
            e->part > 9999u || (e->part != 0 && !e->has_disk))
            return 0;
        if (e->has_disk && e->disk.kind != PART_KIND_MBR &&
            e->disk.kind != PART_KIND_GPT)
            return 0;
    }
    return 1;
}

static int totp_ok(const stationini_totp *t)
{
    if (!t->present)
        return 1;
    return (t->tpm == 12 || t->tpm == 20) && t->pcrs != 0 &&
           (t->srk_sha1 == 0 || (t->srk_sha1 == 1 && t->tpm == 12)) &&
           (t->pcrs >> STATIONINI_TOTP_PCRS) == 0 && totp_key_ok(t) &&
           str_text_ok(t->label, sizeof t->label);
}

static void w_hex(wbuf *w, uint32_t v, unsigned digits)
{
    static const char xd[] = "0123456789abcdef";
    char tmp[9];
    unsigned i;

    for (i = 0; i < digits && i < 8u; i++)
        tmp[i] = xd[(v >> (4u * (digits - 1u - i))) & 0xFu];
    tmp[i] = '\0';
    w_str(w, tmp);
}

static void w_totp(wbuf *w, const stationini_totp *t)
{
    unsigned i;
    int first = 1;

    w_str(w, "[totp]\n");
    w_kv(w, "tpm", t->tpm == 20 ? "2.0" : "1.2");
    w_str(w, "index = 0x");
    w_hex(w, t->index, 8);
    w_str(w, "\npcrs = ");
    for (i = 0; i < STATIONINI_TOTP_PCRS; i++) {
        if ((t->pcrs & (1u << i)) == 0)
            continue;
        if (!first)
            w_str(w, ", ");
        w_uint(w, i);
        first = 0;
    }
    w_str(w, "\nkey = ");
    for (i = 0; i < t->key_len; i++)
        w_hex(w, t->key[i], 2);
    w_str(w, "\n");
    if (t->label[0] != '\0')
        w_kv(w, "label", t->label);
    if (t->srk_sha1)
        w_kv(w, "srkauth", "sha1-zero");
}

size_t stationini_format_totp(const stationini_totp *t, char *buf,
                              size_t cap)
{
    wbuf w;

    if (t == (const stationini_totp *)0 || buf == (char *)0 || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!t->present || !totp_ok(t))
        return 0;
    w.at = buf;
    w.left = cap;
    w.over = 0;
    w_totp(&w, t);
    if (w.over) {
        buf[0] = '\0';
        return 0;
    }
    *w.at = '\0';
    return (size_t)(w.at - buf);
}

size_t stationini_drop_section(const char *text, size_t len,
                               const char *name, char *out, size_t cap)
{
    const char *p;
    const char *end;
    size_t n = 0;
    int dropping = 0;

    if (text == (const char *)0 || name == (const char *)0 ||
        out == (char *)0 || cap == 0)
        return (size_t)-1;
    p = text;
    end = text + len;
    while (p < end) {
        const char *ls = p;
        const char *le;
        const char *a;
        const char *b;

        while (p < end && *p != '\n')
            p++;
        if (p < end)
            p++;
        le = p;
        /* A header line, comments and spaces aside, starts or ends the
         * section; every other line follows the header above it. */
        a = ls;
        b = le;
        while (a < b && (is_space(*a) || *a == '\n'))
            a++;
        if (a < b && *a == '[') {
            const char *c = a + 1;

            while (c < b && *c != ']')
                c++;
            if (c < b) {
                const char *na = a + 1;
                const char *nb = c;

                while (na < nb && is_space(*na))
                    na++;
                while (nb > na && is_space(nb[-1]))
                    nb--;
                dropping = word_eq(na, nb, name);
            }
        }
        if (dropping)
            continue;
        if ((size_t)(le - ls) >= cap - n)
            return (size_t)-1;
        memcpy(out + n, ls, (size_t)(le - ls));
        n += (size_t)(le - ls);
    }
    out[n] = '\0';
    return n;
}

static const char *tri_text(stationini_tri t)
{
    return t == STATIONINI_TRI_YES ? "yes" : "no";
}

size_t stationini_format(const stationini *c, char *buf, size_t cap)
{
    wbuf w;
    unsigned i;

    if (c == (const stationini *)0 || buf == (char *)0 || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!format_ok(c) || !totp_ok(&c->totp))
        return 0;
    w.at = buf;
    w.left = cap;
    w.over = 0;

    if (c->default_name[0] != '\0' || c->timeout != 0 || c->n_order != 0 ||
        c->poweroff_set) {
        w_str(&w, "[general]\n");
        if (c->default_name[0] != '\0')
            w_kv(&w, "default", c->default_name);
        if (c->timeout != 0) {
            w_str(&w, "timeout = ");
            w_uint(&w, c->timeout);
            w_str(&w, "\n");
        }
        if (c->poweroff_set) {
            w_str(&w, "poweroff = ");
            w_uint(&w, c->poweroff);
            w_str(&w, "\n");
        }
        if (c->n_order != 0) {
            w_str(&w, "order = ");
            for (i = 0; i < c->n_order; i++) {
                if (i != 0)
                    w_str(&w, ", ");
                w_str(&w, c->order[i]);
            }
            w_str(&w, "\n");
        }
    }
    if (c->totp.present) {
        if (w.at != buf)
            w_str(&w, "\n");
        w_totp(&w, &c->totp);
    }
    for (i = 0; i < c->n_entries; i++) {
        const stationini_entry *e = &c->entries[i];

        if (w.at != buf)
            w_str(&w, "\n");
        w_str(&w, "[");
        w_str(&w, e->name);
        w_str(&w, "]\n");
        if (e->has_disk) {
            char id[PART_DISK_ID_TEXT];

            part_disk_id_format(&e->disk, id);
            w_kv(&w, "disk", id);
        }
        if (e->part != 0) {
            w_str(&w, "part = ");
            w_uint(&w, e->part);
            w_str(&w, "\n");
        }
        if (e->uki[0] != '\0')
            w_kv(&w, "uki", e->uki);
        if (e->efi[0] != '\0')
            w_kv(&w, "efi", e->efi);
        if (e->method != STATIONINI_METHOD_UNSET)
            w_kv(&w, "method", e->method == STATIONINI_METHOD_EFI ?
                                   "efi" : "firmware");
        if (e->swap != STATIONINI_TRI_UNSET)
            w_kv(&w, "swap", tri_text(e->swap));
        if (e->label[0] != '\0')
            w_kv(&w, "label", e->label);
        if (e->hide != STATIONINI_TRI_UNSET)
            w_kv(&w, "hide", tri_text(e->hide));
    }
    if (w.over) {
        buf[0] = '\0';
        return 0;
    }
    *w.at = '\0';
    return (size_t)(w.at - buf);
}
