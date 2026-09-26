/* falcon jump: JUMP.INI. See jumpini.h.
 *
 * Freestanding: stage 3 has no libc, so the few string routines this needs
 * are here, and the error sentence is built by hand rather than with
 * snprintf.
 */

#include <jump/jumpini.h>

#include <stdint.h>

static const char *const k_default_names[] = {
    "", "hdd", "memtest", "cd", "usb"
};

const char *jump_default_name(jump_default d)
{
    if ((unsigned)d >= sizeof k_default_names / sizeof k_default_names[0])
        return "";
    return k_default_names[d];
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* [a, b) against a NUL-terminated lower-case word, ignoring case. */
static int word_eq(const char *a, const char *b, const char *word)
{
    while (a < b && *word != '\0') {
        if (lower(*a) != *word)
            return 0;
        a++;
        word++;
    }
    return a == b && *word == '\0';
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

/* [a, b), shortened to what fits and never past 24 characters, so a line of
 * garbage cannot push the rest of the sentence off the screen. */
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

static int fail(jump_cfg *out, unsigned line, const char *what,
                const char *a, const char *b, const char *after)
{
    sbuf s;

    out->def = JUMP_DEF_NONE;
    out->timeout = 0;
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
    return -1;
}

static int fail_plain(jump_cfg *out, unsigned line, const char *what)
{
    return fail(out, line, what, (const char *)0, (const char *)0, "");
}

static int parse_default(const char *a, const char *b, jump_default *d)
{
    unsigned i;

    for (i = 1; i < sizeof k_default_names / sizeof k_default_names[0]; i++) {
        if (word_eq(a, b, k_default_names[i])) {
            *d = (jump_default)i;
            return 0;
        }
    }
    return -1;
}

static int parse_seconds(const char *a, const char *b, unsigned *v)
{
    uint32_t n = 0;

    if (a == b)
        return -1;
    for (; a < b; a++) {
        if (*a < '0' || *a > '9')
            return -1;
        n = n * 10u + (uint32_t)(*a - '0');
        if (n > JUMP_TIMEOUT_MAX)
            return -1;
    }
    *v = (unsigned)n;
    return 0;
}

int jump_ini_parse(const char *text, size_t len, jump_cfg *out)
{
    const char *p = text;
    const char *end = text + len;
    unsigned line = 0;
    int have_def = 0;
    int have_timeout = 0;
    unsigned timeout_line = 0;

    if (out == (jump_cfg *)0)
        return -1;
    out->def = JUMP_DEF_NONE;
    out->timeout = 0;
    out->err_line = 0;
    out->why[0] = '\0';
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

        line++;
        while (p < end && *p != '\n')
            p++;
        le = p;
        if (p < end)
            p++;

        /* A comment runs to the end of the line. */
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

        if (word_eq(ka, kb, "default")) {
            if (have_def)
                return fail_plain(out, line, "default is given twice");
            if (parse_default(va, vb, &out->def) != 0)
                return fail(out, line, "default", va, vb,
                            " is not hdd, memtest, cd or usb");
            have_def = 1;
        } else if (word_eq(ka, kb, "timeout")) {
            if (have_timeout)
                return fail_plain(out, line, "timeout is given twice");
            if (parse_seconds(va, vb, &out->timeout) != 0)
                return fail(out, line, "timeout", va, vb,
                            " is not a number of seconds up to 3600");
            have_timeout = 1;
            timeout_line = line;
        } else {
            return fail(out, line, "unknown key", ka, kb, "");
        }
    }

    if (have_timeout && out->timeout != 0 && !have_def)
        return fail_plain(out, timeout_line,
                          "timeout is set but default is not");
    return 0;
}
