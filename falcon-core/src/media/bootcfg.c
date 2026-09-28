/* Reading an ISO's boot configuration. See falcon-core/src/media/bootcfg.h
 * for what is read, in what order, and what is deliberately not.
 *
 * The GRUB half is a tokenizer and a block stack, not an interpreter. GRUB's
 * script language has quoting, variables, conditionals, loops and functions;
 * a distro's generated grub.cfg uses them to pick a theme, a gfxmode and a
 * platform, and uses plain menuentry blocks for what boots. So: quoting and
 * variable expansion are done properly, because they change what the kernel
 * sees; conditionals are stepped through (both branches read), because
 * entries sit inside `if` blocks that would be true on the machine that boots
 * them; a for loop over words this pass knows is run, because Manjaro builds
 * its cmdline in one; while/until and function bodies are skipped, because
 * their text is a template rather than an entry. What cannot be known
 * without running GRUB (a variable set by `search --set` or `probe --set`, a
 * glob over the ISO's files) stays unresolved and marks the entry that used
 * it.
 */

#include <media/bootcfg.h>

#include <lib/string.h>

/* Bounds on what one ISO may make the parser do. */
#define DEPTH_MAX     6u    /* nested source/configfile */
#define FILES_MAX     24u   /* files read in total */
#define VARS_MAX      64u
#define VAR_NAME_MAX  32u
#define VAR_VALUE_MAX 256u
#define WORDS_MAX     160u  /* words in one command */
#define WORD_MAX      512u  /* one word, after expansion */
#define BLOCKS_MAX    32u   /* nested {} */
#define SDBOOT_MAX    24u   /* loader entries considered */
#define LOOP_ITER_MAX 32u   /* words one for loop runs its body for */
#define POS_MAX       9u    /* a menuentry's $1..$9 */

/* ---------------------------------------------------------- strings */

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int ieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (lower(*a) != lower(*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Bounded append; sets *trunc when something did not fit. */
static void app(char *dst, size_t cap, const char *src, size_t n,
                uint8_t *trunc)
{
    size_t d = slen(dst);
    size_t i;

    for (i = 0; i < n; i++) {
        if (d + 1u >= cap) {
            if (trunc != (uint8_t *)0)
                *trunc = 1;
            break;
        }
        dst[d++] = src[i];
    }
    dst[d] = '\0';
}

/* Bounded copy of a NUL-terminated string, stopping at its terminator
 * rather than at a length measured beforehand. */
static void cpy(char *dst, size_t cap, const char *src, uint8_t *trunc)
{
    size_t d = 0;

    while (src[d] != '\0') {
        if (d + 1u >= cap) {
            if (trunc != (uint8_t *)0)
                *trunc = 1;
            break;
        }
        dst[d] = src[d];
        d++;
    }
    dst[d] = '\0';
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t h = slen(hay), n = slen(needle), i, j;

    for (i = 0; i + n <= h; i++) {
        for (j = 0; j < n && lower(hay[i + j]) == lower(needle[j]); j++)
            ;
        if (j == n)
            return 1;
    }
    return 0;
}

/* --------------------------------------------------------- the parser */

typedef struct {
    char    name[VAR_NAME_MAX];
    char    value[VAR_VALUE_MAX];
    uint8_t runtime;   /* computed by GRUB at run time: value unknowable */
    /* Set inside a branch whose condition this pass could not decide: the
     * value holds for the rest of that branch, and becomes unknowable when
     * the branch is left (cond_leave). The number of conditions open at the
     * assignment; 0 when the value is certain. */
    uint8_t pending;
} var;

/* One open `if`: what the condition of the branch now being read came to,
 * and whether an earlier branch of the same if was already true. */
typedef struct {
    uint8_t state;     /* COND_FALSE, COND_TRUE, COND_UNKNOWN */
    uint8_t taken;     /* an earlier branch was true */
} cond;

#define COND_FALSE   ((uint8_t)0)
#define COND_TRUE    ((uint8_t)1)
#define COND_UNKNOWN ((uint8_t)2)
#define CONDS_MAX    16u

typedef enum {
    BLK_PLAIN = 0,   /* an if/else body or an unknown { } */
    BLK_MENU,
    BLK_SUBMENU,
    BLK_SKIP         /* a function body, or a loop */
} blk_kind;

typedef struct {
    const bootcfg_src *src;
    char   *scratch;
    size_t  used, cap;
    unsigned files;
    int     loop;             /* reading a loopback.cfg */
    int     nospace;

    var     vars[VARS_MAX];
    size_t  n_vars;

    blk_kind blk[BLOCKS_MAX];
    size_t   n_blk;
    unsigned submenus;        /* open submenu blocks */
    unsigned skips;           /* open skipped blocks */
    unsigned loops;           /* open for/while ... done */

    cond     conds[CONDS_MAX];
    size_t   n_conds;
    int      iso_set;         /* ${iso_path} counts as non-empty */
    int      taint;           /* everything read is run-time dependent: an
                                 eval whose text used such a value */

    bootcfg_entry *cur;       /* the entry an open menuentry fills */
    bootcfg_result *res;

    char    dir[BOOTCFG_PATH_MAX];  /* isolinux: the config's directory */
} parser;

/* The parser is large (variables, words) and the loader has no allocator,
 * so it is one static object; bootcfg_read is not reentrant. */
static parser g_p;

static const var *var_get(parser *p, const char *name)
{
    size_t i;

    for (i = 0; i < p->n_vars; i++) {
        if (ieq(p->vars[i].name, name))
            return &p->vars[i];
    }
    return (const var *)0;
}

static void var_set_rt(parser *p, const char *name, size_t nlen,
                       const char *value, uint8_t runtime)
{
    size_t i;
    char n[VAR_NAME_MAX];

    if (nlen == 0 || nlen >= VAR_NAME_MAX)
        return;
    memcpy(n, name, nlen);
    n[nlen] = '\0';
    for (i = 0; i < p->n_vars; i++) {
        if (ieq(p->vars[i].name, n))
            break;
    }
    if (i == p->n_vars) {
        if (p->n_vars >= VARS_MAX)
            return;
        p->n_vars++;
        cpy(p->vars[i].name, VAR_NAME_MAX, n, (uint8_t *)0);
    }
    cpy(p->vars[i].value, VAR_VALUE_MAX, value, (uint8_t *)0);
    p->vars[i].runtime = runtime;
    p->vars[i].pending = 0;
}

/* A variable this module sets itself, by a NUL-terminated name. */
static void var_set(parser *p, const char *name, const char *value)
{
    var_set_rt(p, name, slen(name), value, 0);
}

/* Reads a whole file into the next free stretch of scratch. The stretch is
 * released when the caller is done with the file (scratch is a stack, since
 * a sourced file is finished before the file that sourced it). */
static char *file_load(parser *p, const char *path, size_t *len)
{
    char *buf;
    size_t n = 0;

    if (p->files >= FILES_MAX || p->used + 1u >= p->cap)
        return (char *)0;
    buf = p->scratch + p->used;
    if (p->src->read(p->src->ctx, path, buf, p->cap - p->used - 1u, &n) != 0) {
        /* The one failure worth telling apart: a file that is there and did
         * not fit, which is the caller's scratch and not the ISO. */
        if (n > p->cap - p->used - 1u)
            p->nospace = 1;
        return (char *)0;
    }
    if (n > p->cap - p->used - 1u) {
        p->nospace = 1;
        return (char *)0;
    }
    buf[n] = '\0';
    p->files++;
    p->used += n + 1u;
    *len = n;
    return buf;
}

static void file_release(parser *p, const char *buf, size_t len)
{
    if (buf == p->scratch + p->used - len - 1u)
        p->used -= len + 1u;
}

/* Runs of '/' as one, in place: GRUB and syslinux read "/boot//nix/x" as
 * "/boot/nix/x" (NixOS writes the former), and a file lookup need not. */
static void squash_slashes(char *s)
{
    size_t r = 0, w = 0;

    for (; s[r] != '\0'; r++) {
        if (s[r] == '/' && w > 0 && s[w - 1u] == '/')
            continue;
        s[w++] = s[r];
    }
    s[w] = '\0';
}

/* "(hd0,gpt2)/boot/x" and "($root)/boot/x" are "/boot/x"; a relative path
 * gets its leading slash. */
static void clean_path(const char *in, char *out, size_t cap, uint8_t *trunc)
{
    const char *s = in;

    if (*s == '(') {
        while (*s != '\0' && *s != ')')
            s++;
        if (*s == ')')
            s++;
    }
    out[0] = '\0';
    if (*s != '/')
        app(out, cap, "/", 1, trunc);
    app(out, cap, s, slen(s), trunc);
    squash_slashes(out);
}

/* ---------------------------------------------------------------- GRUB */

/* One GRUB word, with its quoting undone and its variables expanded into
 * out. Returns the input position after it. *unres is set when a variable
 * had no value this parser could know. */
static const char *grub_word(parser *p, const char *s, const char *end,
                             char *out, size_t cap, uint8_t *unres,
                             uint8_t *trunc)
{
    char q = 0;

    out[0] = '\0';
    while (s < end) {
        char c = *s;

        /* An unquoted '}' closes a block even when written against the
         * word before it ("{menu_reload}"); a variable's braces never get
         * here, since the '$' branch below consumes them. */
        if (q == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                       c == ';' || c == '}'))
            break;
        if (q == 0 && (c == '"' || c == '\'')) {
            q = c;
            s++;
            continue;
        }
        if (q != 0 && c == q) {
            q = 0;
            s++;
            continue;
        }
        if (c == '\\' && q != '\'' && s + 1 < end) {
            if (s[1] == '\n') {
                s += 2;
                continue;
            }
            app(out, cap, s + 1, 1, trunc);
            s += 2;
            continue;
        }
        if (c == '$' && q != '\'' && s + 1 < end) {
            char name[VAR_NAME_MAX];
            size_t n = 0;
            const var *v;

            s++;
            if (*s == '{') {
                s++;
                while (s < end && *s != '}' && n + 1u < sizeof name)
                    name[n++] = *s++;
                while (s < end && *s != '}')
                    s++;
                if (s < end)
                    s++;
            } else {
                while (s < end && n + 1u < sizeof name &&
                       ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                        (*s >= '0' && *s <= '9') || *s == '_'))
                    name[n++] = *s++;
            }
            name[n] = '\0';
            if (n == 0) {
                app(out, cap, "$", 1, trunc);
                continue;
            }
            if (ieq(name, "iso_path")) {
                app(out, cap, BOOTCFG_ISO_TOKEN, slen(BOOTCFG_ISO_TOKEN),
                    trunc);
                continue;
            }
            v = var_get(p, name);
            /* Unset expands to nothing, as in GRUB. A value GRUB computes
             * at run time is unknowable here, and says so. */
            if (v != (const var *)0) {
                app(out, cap, v->value, slen(v->value), trunc);
                if (v->runtime && unres != (uint8_t *)0)
                    *unres = 1;
            }
            continue;
        }
        app(out, cap, s, 1, trunc);
        s++;
    }
    return s;
}

static int is_word(const char *w, const char *kw)
{
    return ieq(w, kw);
}

static bootcfg_entry *entry_new(parser *p, const char *title)
{
    bootcfg_entry *e;

    if (p->res->n >= BOOTCFG_ENTRIES_MAX)
        return (bootcfg_entry *)0;
    e = &p->res->entry[p->res->n++];
    memset(e, 0, sizeof(*e));
    cpy(e->title, sizeof e->title, title, &e->truncated);
    e->nested = (uint8_t)(p->submenus > 0);
    e->loop = (uint8_t)p->loop;
    return e;
}

/* A command's words: the static buffers the tokenizer fills per command,
 * and for each whether its expansion used a value computed at run time. */
static char g_words[WORDS_MAX][WORD_MAX];
static uint8_t g_wunres[WORDS_MAX];

static void grub_parse(parser *p, const char *buf, size_t len,
                       unsigned depth);

static void grub_include(parser *p, const char *arg, unsigned depth)
{
    char path[BOOTCFG_PATH_MAX];
    char *buf;
    size_t len = 0;

    if (depth + 1u >= DEPTH_MAX)
        return;
    clean_path(arg, path, sizeof path, (uint8_t *)0);
    buf = file_load(p, path, &len);
    if (buf == (char *)0)
        return;
    grub_parse(p, buf, len, depth + 1u);
    file_release(p, buf, len);
}

/* The title of a menuentry/submenu: its first word that is not an option.
 * Options that take a value (--class x) consume the next word too. */
static const char *grub_title(size_t nw)
{
    size_t i;

    for (i = 1; i < nw; i++) {
        const char *w = g_words[i];

        if (w[0] == '-' && w[1] == '-') {
            if (is_word(w, "--class") || is_word(w, "--id") ||
                is_word(w, "--hotkey") || is_word(w, "--users"))
                i++;
            continue;
        }
        if (w[0] == '{')
            break;
        return w;
    }
    return "";
}

/* A menuentry's body sees its arguments as $1 (the title), $2 and on: the
 * words that are not options. Manjaro passes its whole cmdline that way and
 * writes "linux /boot/vmlinuz-$2". */
static void menu_args(parser *p, size_t nw)
{
    size_t i;
    unsigned k = 0;

    for (i = 1; i < nw && k < POS_MAX; i++) {
        const char *w = g_words[i];
        char name[2];

        if (w[0] == '-' && w[1] == '-') {
            if (is_word(w, "--class") || is_word(w, "--id") ||
                is_word(w, "--hotkey") || is_word(w, "--users"))
                i++;
            continue;
        }
        name[0] = (char)('1' + k);
        name[1] = '\0';
        var_set_rt(p, name, 1, w, g_wunres[i]);
        k++;
    }
}

static void menu_args_clear(parser *p)
{
    unsigned k;

    for (k = 0; k < POS_MAX; k++) {
        char name[2];

        name[0] = (char)('1' + k);
        name[1] = '\0';
        if (var_get(p, name) != (const var *)0)
            var_set_rt(p, name, 1, "", 0);
    }
}

static void blk_push(parser *p, blk_kind k)
{
    if (p->n_blk < BLOCKS_MAX)
        p->blk[p->n_blk] = k;
    p->n_blk++;
    if (k == BLK_SUBMENU)
        p->submenus++;
    if (k == BLK_SKIP)
        p->skips++;
}

static void blk_pop(parser *p)
{
    blk_kind k;

    if (p->n_blk == 0)
        return;
    p->n_blk--;
    k = (p->n_blk < BLOCKS_MAX) ? p->blk[p->n_blk] : BLK_PLAIN;
    if (k == BLK_SUBMENU && p->submenus > 0)
        p->submenus--;
    if (k == BLK_SKIP && p->skips > 0)
        p->skips--;
    if (k == BLK_MENU) {
        p->cur = (bootcfg_entry *)0;
        menu_args_clear(p);
    }
}

/* Is every enclosing `if` branch possibly taken? A branch known false makes
 * the commands in it, blocks included, as good as absent. */
static int cond_active(const parser *p)
{
    size_t i;

    for (i = 0; i < p->n_conds && i < CONDS_MAX; i++) {
        if (p->conds[i].state == COND_FALSE)
            return 0;
    }
    return 1;
}

/* [ -z "$v" ] and [ -n "$v" ], after expansion; everything else is unknown.
 * ${iso_path} is empty or not as the pass says, and a run-time value is
 * unknown by definition. */
static int word_empty(const parser *p, const char *v)
{
    if (ieq(v, BOOTCFG_ISO_TOKEN))
        return !p->iso_set;
    return v[0] == '\0';
}

/* [ OP ARG ] for the unary tests this pass can answer. */
static uint8_t cond_unary(parser *p, const char *op, const char *arg)
{
    int empty = word_empty(p, arg);

    if (ieq(op, "-z"))
        return empty ? COND_TRUE : COND_FALSE;
    if (ieq(op, "-n"))
        return empty ? COND_FALSE : COND_TRUE;
    /* A file on the medium: the one thing about the boot environment this
     * pass can ask, when the provider can answer. GRUB resolves a bare name
     * against $root, which on a CD is the ISO's root (Proxmox gates its
     * automated installer on one). */
    if ((ieq(op, "-f") || ieq(op, "-e")) &&
        p->src->exists != (int (*)(void *, const char *))0 &&
        !ieq(arg, BOOTCFG_ISO_TOKEN)) {
        char path[BOOTCFG_PATH_MAX];
        uint8_t trunc = 0;

        clean_path(arg, path, sizeof path, &trunc);
        if (trunc)
            return (uint8_t)COND_UNKNOWN;
        return p->src->exists(p->src->ctx, path) ? COND_TRUE : COND_FALSE;
    }
    return (uint8_t)COND_UNKNOWN;
}

static uint8_t cond_eval(parser *p, size_t first, size_t nw)
{
    size_t i;

    if (nw < first + 3u || !ieq(g_words[first], "[") ||
        !ieq(g_words[nw - 1u], "]"))
        return (uint8_t)COND_UNKNOWN;
    for (i = first; i < nw; i++) {
        if (g_wunres[i])
            return (uint8_t)COND_UNKNOWN;
    }
    if (nw - first == 4u)
        return cond_unary(p, g_words[first + 1u], g_words[first + 2u]);
    if (nw - first == 5u && ieq(g_words[first + 1u], "!")) {
        uint8_t c = cond_unary(p, g_words[first + 2u], g_words[first + 3u]);

        return c == COND_UNKNOWN ? c
               : (c == COND_TRUE ? (uint8_t)COND_FALSE : (uint8_t)COND_TRUE);
    }
    if (nw - first == 5u) {
        const char *a = g_words[first + 1u], *op = g_words[first + 2u];
        const char *b = g_words[first + 3u];
        int same;

        /* ${iso_path} compares as unknown: its value is the one thing
         * about the boot this pass does not know. */
        if (ieq(a, BOOTCFG_ISO_TOKEN) || ieq(b, BOOTCFG_ISO_TOKEN))
            return (uint8_t)COND_UNKNOWN;
        same = (slen(a) == slen(b)) && memcmp(a, b, slen(a)) == 0;
        if (ieq(op, "=") || ieq(op, "=="))
            return same ? COND_TRUE : COND_FALSE;
        if (ieq(op, "!="))
            return same ? COND_FALSE : COND_TRUE;
    }
    return (uint8_t)COND_UNKNOWN;
}

/* Inside a branch whose condition could not be decided, an assignment may
 * or may not happen, so its value is as unknowable as a run-time one. */
static uint8_t cond_uncertain(const parser *p)
{
    size_t i;

    for (i = 0; i < p->n_conds && i < CONDS_MAX; i++) {
        if (p->conds[i].state == COND_UNKNOWN)
            return 1;
    }
    return 0;
}

/* NAME=VALUE in the current branch. Under a condition this pass could not
 * decide, the value holds for the rest of the branch (an entry defined
 * there sees it) and becomes unknowable once the branch is left, unless it
 * changed nothing: a variable already holding VALUE, unset counting as
 * empty, holds it either way (Manjaro's auto="${auto}"). */
static void assign(parser *p, const char *name, size_t nlen,
                   const char *value, uint8_t unres)
{
    char n[VAR_NAME_MAX];
    const var *v;
    size_t i;

    if (nlen == 0 || nlen >= VAR_NAME_MAX)
        return;
    memcpy(n, name, nlen);
    n[nlen] = '\0';
    v = var_get(p, n);
    if (!unres && cond_uncertain(p) &&
        (v == (const var *)0 ? value[0] == '\0'
                             : (!v->runtime && v->pending == 0 &&
                                slen(v->value) == slen(value) &&
                                memcmp(v->value, value, slen(value)) == 0)))
        return;
    var_set_rt(p, name, nlen, value, unres);
    if (unres || !cond_uncertain(p))
        return;
    for (i = 0; i < p->n_vars; i++) {
        if (ieq(p->vars[i].name, n))
            p->vars[i].pending = (uint8_t)(p->n_conds < CONDS_MAX
                                               ? p->n_conds : CONDS_MAX);
    }
}

/* The branch of condition index m is being left (an elif, an else, a fi, or
 * the end of a loop's unknown pass). What it assigned becomes unknowable if
 * the branch was undecided; if it was known to run, the values stand, still
 * pending on whatever undecided condition encloses them. */
static void cond_leave(parser *p, size_t m, int popping)
{
    uint8_t undecided;
    size_t i;

    if (m >= CONDS_MAX)
        return;
    undecided = (uint8_t)(p->conds[m].state == COND_UNKNOWN);
    for (i = 0; i < p->n_vars; i++) {
        var *v = &p->vars[i];

        if (v->pending <= m)
            continue;
        if (undecided) {
            v->runtime = 1;
            v->pending = 0;
        } else if (popping) {
            size_t k;

            v->pending = 0;
            for (k = 0; k < m; k++) {
                if (p->conds[k].state == COND_UNKNOWN)
                    v->pending = (uint8_t)m;
            }
        }
    }
}

/* `--set=NAME` or `--set NAME` anywhere in a command marks NAME as a value
 * GRUB computes at run time (search, probe, regexp all take it). */
static void mark_runtime(parser *p, size_t nw)
{
    size_t i;
    const char *name;

    for (i = 1; i < nw; i++) {
        const char *w = g_words[i];

        if (w[0] != '-' || w[1] != '-' || lower(w[2]) != 's' ||
            lower(w[3]) != 'e' || lower(w[4]) != 't')
            continue;
        name = (const char *)0;
        if (w[5] == '=')
            name = w + 6;
        else if (w[5] == '\0' && i + 1u < nw)
            name = g_words[i + 1u];
        /* $root is only ever a device prefix, "($root)/boot/x", which a
         * path here drops; what search finds for it changes nothing. */
        if (name != (const char *)0 && !ieq(name, "root"))
            var_set_rt(p, name, slen(name), "", 1);
    }
}

/* eval STRING: STRING, already expanded, parsed as script. Copied out of
 * the word buffers first, which the nested parse reuses. */
static void grub_eval(parser *p, size_t nw, unsigned depth)
{
    size_t i, len = 0;
    char *buf;
    uint8_t tainted = 0;

    if (depth + 1u >= DEPTH_MAX || p->used >= p->cap)
        return;
    buf = p->scratch + p->used;
    for (i = 1; i < nw; i++) {
        size_t wl = slen(g_words[i]);

        if (p->used + len + wl + 2u > p->cap)
            return;
        if (i > 1)
            buf[len++] = ' ';
        memcpy(buf + len, g_words[i], wl);
        len += wl;
    }
    buf[len] = '\0';
    p->used += len + 1u;
    for (i = 1; i < nw; i++)
        tainted |= g_wunres[i];
    if (tainted)
        p->taint++;
    grub_parse(p, buf, len, depth + 1u);
    if (tainted)
        p->taint--;
    p->used -= len + 1u;
}

/* One command, already split into words (quotes undone, variables
 * expanded). opens is 1 when the command ended at a '{'. */
static void grub_command(parser *p, size_t nw, int opens, uint8_t unres,
                         uint8_t trunc, unsigned depth)
{
    const char *w0 = g_words[0];

    /* Conditionals first: they decide whether anything else is read. */
    if (!opens && nw > 0 && p->skips == 0 && p->loops == 0) {
        if (is_word(w0, "if")) {
            if (p->n_conds < CONDS_MAX) {
                cond *c = &p->conds[p->n_conds];

                c->state = cond_active(p) ? cond_eval(p, 1, nw)
                                          : (uint8_t)COND_FALSE;
                c->taken = (uint8_t)(c->state == COND_TRUE);
            }
            p->n_conds++;
            return;
        }
        if (is_word(w0, "elif") || is_word(w0, "else")) {
            if (p->n_conds > 0 && p->n_conds <= CONDS_MAX) {
                cond *c = &p->conds[p->n_conds - 1u];
                uint8_t st;

                cond_leave(p, p->n_conds - 1u, 0);

                if (c->taken) {
                    st = COND_FALSE;
                } else if (is_word(w0, "else")) {
                    st = (c->state == COND_UNKNOWN) ? (uint8_t)COND_UNKNOWN
                                                    : (uint8_t)COND_TRUE;
                } else {
                    /* The parent's activity is the outer ones'; this
                     * branch's own state is what is being replaced. */
                    c->state = COND_UNKNOWN;
                    st = cond_active(p) ? cond_eval(p, 1, nw)
                                        : (uint8_t)COND_FALSE;
                }
                c->state = st;
                if (st == COND_TRUE)
                    c->taken = 1;
            }
            if (is_word(w0, "elif"))
                mark_runtime(p, nw);
            return;
        }
        if (is_word(w0, "fi")) {
            if (p->n_conds > 0) {
                cond_leave(p, p->n_conds - 1u, 1);
                p->n_conds--;
            }
            return;
        }
        if (is_word(w0, "then"))
            return;
    }

    if (opens) {
        if (p->skips > 0 || p->loops > 0 || !cond_active(p) ||
            is_word(w0, "function")) {
            blk_push(p, BLK_SKIP);
        } else if (is_word(w0, "menuentry")) {
            blk_push(p, BLK_MENU);
            p->cur = entry_new(p, grub_title(nw));
            menu_args(p, nw);
        } else if (is_word(w0, "submenu")) {
            blk_push(p, BLK_SUBMENU);
        } else {
            blk_push(p, BLK_PLAIN);
        }
        return;
    }
    if (nw == 0)
        return;

    /* Loops are skipped whole: their bodies name the loop variable. */
    if (is_word(w0, "for") || is_word(w0, "while") || is_word(w0, "until")) {
        p->loops++;
        return;
    }
    if (is_word(w0, "done")) {
        if (p->loops > 0)
            p->loops--;
        return;
    }
    if (p->skips > 0 || p->loops > 0 || !cond_active(p))
        return;

    mark_runtime(p, nw);

    if (p->cur != (bootcfg_entry *)0) {
        bootcfg_entry *e = p->cur;

        if (is_word(w0, "linux") || is_word(w0, "linuxefi") ||
            is_word(w0, "linux16") || is_word(w0, "kernel")) {
            size_t i, kl = 0;
            const char *kw;

            if (nw < 2)
                return;
            kw = g_words[1];
            if (g_wunres[1])
                e->kernel_unresolved = 1;
            /* An unquoted expansion splits into words in GRUB, so a kernel
             * word with blanks in it is the path and then arguments
             * ("/boot/vmlinuz-$2" on Manjaro). No kernel path has one. */
            while (kw[kl] != '\0' && kw[kl] != ' ' && kw[kl] != '\t')
                kl++;
            {
                char kp[BOOTCFG_PATH_MAX];
                size_t c = kl < sizeof kp ? kl : sizeof kp - 1u;

                memcpy(kp, kw, c);
                kp[c] = '\0';
                if (c < kl)
                    e->truncated = 1;
                clean_path(kp, e->kernel, sizeof e->kernel, &e->truncated);
            }
            e->cmdline[0] = '\0';
            while (kw[kl] == ' ' || kw[kl] == '\t')
                kl++;
            if (kw[kl] != '\0') {
                app(e->cmdline, sizeof e->cmdline, kw + kl, slen(kw + kl),
                    &e->truncated);
                if (g_wunres[1])
                    e->unresolved = 1;
            }
            for (i = 2; i < nw; i++) {
                const char *a = g_words[i];
                int spaced = 0;
                size_t k;

                if (a[0] == '\0')
                    continue;
                for (k = 0; a[k] != '\0'; k++) {
                    if (a[k] == ' ' || a[k] == '\t')
                        spaced = 1;
                }
                if (e->cmdline[0] != '\0')
                    app(e->cmdline, sizeof e->cmdline, " ", 1, &e->truncated);
                if (spaced)
                    app(e->cmdline, sizeof e->cmdline, "\"", 1,
                        &e->truncated);
                app(e->cmdline, sizeof e->cmdline, a, slen(a), &e->truncated);
                if (spaced)
                    app(e->cmdline, sizeof e->cmdline, "\"", 1,
                        &e->truncated);
                if (g_wunres[i])
                    e->unresolved = 1;
            }
            if (trunc)
                e->truncated = 1;
            e->format = BOOTCFG_FMT_GRUB;
        } else if (is_word(w0, "initrd") || is_word(w0, "initrdefi") ||
                   is_word(w0, "initrd16")) {
            size_t i;

            e->n_initrd = 0;
            for (i = 1; i < nw && e->n_initrd < BOOTCFG_INITRD_MAX; i++) {
                if (g_words[i][0] == '\0')
                    continue;
                clean_path(g_words[i], e->initrd[e->n_initrd],
                           sizeof e->initrd[0], &e->truncated);
                e->n_initrd++;
            }
            if (i < nw)
                e->truncated = 1;
            if (unres)
                e->kernel_unresolved = 1;
        }
        return;
    }

    if (is_word(w0, "source") || is_word(w0, "configfile")) {
        if (nw >= 2 && !unres)
            grub_include(p, g_words[1], depth);
        return;
    }
    if (is_word(w0, "eval")) {
        grub_eval(p, nw, depth);
        return;
    }
    if (is_word(w0, "set") && nw >= 2) {
        const char *eq = g_words[1];
        size_t n = 0;

        while (eq[n] != '\0' && eq[n] != '=')
            n++;
        if (eq[n] == '=')
            assign(p, eq, n, eq + n + 1, unres);
        return;
    }
    /* A bare assignment, "default=archlinux". */
    {
        size_t n = 0;

        while (w0[n] != '\0' && w0[n] != '=' &&
               ((w0[n] >= 'a' && w0[n] <= 'z') || (w0[n] >= 'A' && w0[n] <= 'Z') ||
                (w0[n] >= '0' && w0[n] <= '9') || w0[n] == '_'))
            n++;
        if (n > 0 && w0[n] == '=' && nw == 1)
            assign(p, w0, n, w0 + n + 1, unres);
    }
}

/* ------------------------------------------------------------ for loops
 *
 * for NAME in WORDS; do BODY; done runs BODY once per word, the way GRUB
 * does, when every word is a value this pass knows. A word it cannot know (a
 * run-time value, or a glob over the ISO's files, which GRUB expands against
 * a directory listing) runs BODY once more with NAME unknown and under an
 * undecidable condition, so whatever the body assigns becomes unknown and an
 * entry it builds from NAME is never picked. while and until loops are
 * skipped whole, as before. */

static int word_ch(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* The bare word at s, if any: its length. */
static size_t bare_word(const char *s, const char *end)
{
    size_t n = 0;

    while (s + n < end && word_ch(s[n]))
        n++;
    return n;
}

static int bare_is(const char *s, size_t n, const char *kw)
{
    size_t kl = slen(kw);

    return n == kl && memcmp(s, kw, n) == 0;
}

/* From s, the start of a loop body, finds the "done" that closes it,
 * counting nested loops. Commands are skipped raw: quotes, escapes and
 * comments are honoured so a "done" in a string does not count. Returns the
 * position after "done" and sets *at to its start, or NULL when there is
 * none. */
static const char *find_done(const char *s, const char *end, const char **at)
{
    unsigned depth = 1;

    while (s < end) {
        size_t n;

        while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' ||
                           *s == '\n' || *s == ';' || *s == '{' || *s == '}'))
            s++;
        if (s >= end)
            break;
        if (*s == '#') {
            while (s < end && *s != '\n')
                s++;
            continue;
        }
        n = bare_word(s, end);
        if (bare_is(s, n, "for") || bare_is(s, n, "while") ||
            bare_is(s, n, "until")) {
            depth++;
        } else if (bare_is(s, n, "done")) {
            if (--depth == 0) {
                *at = s;
                return s + n;
            }
        }
        /* The rest of the command. */
        while (s < end && *s != '\n' && *s != ';' && *s != '{' && *s != '}') {
            if (*s == '"' || *s == '\'') {
                char q = *s++;

                while (s < end && *s != q) {
                    if (q == '"' && *s == '\\' && s + 1 < end)
                        s++;
                    s++;
                }
                if (s < end)
                    s++;
                continue;
            }
            if (*s == '\\' && s + 1 < end)
                s++;
            s++;
        }
    }
    return (const char *)0;
}

static int has_glob(const char *w)
{
    for (; *w != '\0'; w++) {
        if (*w == '*' || *w == '?' || *w == '[')
            return 1;
    }
    return 0;
}

static void grub_parse(parser *p, const char *buf, size_t len,
                       unsigned depth);

/* The loop whose "for NAME in WORDS" command is in g_words, with s just past
 * it. Returns where parsing resumes, or NULL when this is not a loop this
 * pass runs (no "do", no "done"), which leaves it to be skipped. */
static const char *grub_for(parser *p, size_t nw, const char *s,
                            const char *end, unsigned depth)
{
    const char *body, *body_end, *after;
    char *words;
    uint8_t unres[WORDS_MAX];
    char name[VAR_NAME_MAX];
    size_t i, used, nl, n;
    unsigned iter = 0;
    int unknown = 0;

    if (nw < 3 || !ieq(g_words[2], "in") || g_wunres[1] ||
        depth + 1u >= DEPTH_MAX)
        return (const char *)0;
    nl = slen(g_words[1]);
    if (nl == 0 || nl >= VAR_NAME_MAX)
        return (const char *)0;
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' ||
                       *s == '\n' || *s == ';'))
        s++;
    n = bare_word(s, end);
    if (!bare_is(s, n, "do"))
        return (const char *)0;
    body = s + n;
    after = find_done(body, end, &body_end);
    if (after == (const char *)0)
        return (const char *)0;

    /* The words and the name are copied out: the body's own commands reuse
     * g_words. */
    memcpy(name, g_words[1], nl + 1u);
    used = p->used;
    words = p->scratch + used;
    for (i = 3; i < nw; i++) {
        size_t wl = slen(g_words[i]);

        if (p->used + wl + 1u > p->cap) {
            p->used = used;
            return (const char *)0;
        }
        memcpy(p->scratch + p->used, g_words[i], wl + 1u);
        p->used += wl + 1u;
        unres[i] = g_wunres[i];
    }

    for (i = 3; i < nw; i++) {
        const char *w = words;

        words += slen(w) + 1u;
        if (unres[i] || has_glob(w)) {
            unknown = 1;
            continue;
        }
        /* An unquoted empty expansion is no word at all in GRUB. */
        if (w[0] == '\0')
            continue;
        if (iter++ >= LOOP_ITER_MAX)
            break;
        var_set_rt(p, name, nl, w, 0);
        grub_parse(p, body, (size_t)(body_end - body), depth + 1u);
    }
    if (unknown && p->n_conds < CONDS_MAX) {
        cond *c = &p->conds[p->n_conds++];

        c->state = COND_UNKNOWN;
        c->taken = 0;
        var_set_rt(p, name, nl, "", 1);
        grub_parse(p, body, (size_t)(body_end - body), depth + 1u);
        cond_leave(p, p->n_conds - 1u, 1);
        p->n_conds--;
    }
    p->used = used;
    return after;
}

/* Splits the buffer into commands. A command ends at a newline or ';',
 * or at a '{' (which opens a block) or a '}' (which closes one). Comments
 * run from an unquoted '#' at the start of a word to the end of the line. */
static void grub_parse(parser *p, const char *buf, size_t len,
                       unsigned depth)
{
    const char *s = buf;
    const char *end = buf + len;

    while (s < end) {
        size_t nw = 0;
        uint8_t unres = 0, trunc = 0;
        int opens = 0, closes = 0;

        for (;;) {
            while (s < end && (*s == ' ' || *s == '\t' || *s == '\r'))
                s++;
            if (s >= end)
                break;
            if (*s == '\n' || *s == ';') {
                s++;
                break;
            }
            if (*s == '#') {
                while (s < end && *s != '\n')
                    s++;
                continue;
            }
            if (*s == '{') {
                s++;
                opens = 1;
                break;
            }
            if (*s == '}') {
                s++;
                closes = 1;
                break;
            }
            if (nw < WORDS_MAX) {
                uint8_t wu = 0;

                s = grub_word(p, s, end, g_words[nw], WORD_MAX, &wu, &trunc);
                if (p->taint > 0)
                    wu = 1;
                g_wunres[nw] = wu;
                unres = (uint8_t)(unres | wu);
                nw++;
            } else {
                char sink[WORD_MAX];

                s = grub_word(p, s, end, sink, sizeof sink, &unres, &trunc);
                trunc = 1;
            }
        }
        if (!opens && nw > 0 && is_word(g_words[0], "for") &&
            p->skips == 0 && p->loops == 0 && cond_active(p)) {
            const char *resume = grub_for(p, nw, s, end, depth);

            if (resume != (const char *)0) {
                s = resume;
                continue;
            }
        }
        grub_command(p, nw, opens, unres, trunc, depth);
        if (closes)
            blk_pop(p);
    }
}

/* ---------------------------------------------------------- systemd-boot */

typedef struct {
    char names[SDBOOT_MAX][BOOTCFG_TITLE_MAX];
    size_t n;
} sd_list;

static sd_list g_sd;

static int sd_collect(void *arg, const char *name)
{
    sd_list *l = arg;
    size_t n = slen(name);

    if (n < 6 || !ieq(name + n - 5, ".conf"))
        return 0;
    if (l->n < SDBOOT_MAX && n < BOOTCFG_TITLE_MAX) {
        cpy(l->names[l->n], BOOTCFG_TITLE_MAX, name, (uint8_t *)0);
        l->n++;
    }
    return 0;
}

static int name_lt(const char *a, const char *b)
{
    while (*a != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower(*a) < lower(*b);
}

static void sd_parse(parser *p, const char *buf, size_t len)
{
    bootcfg_entry *e = entry_new(p, "");
    const char *s = buf, *end = buf + len;

    if (e == (bootcfg_entry *)0)
        return;
    e->format = BOOTCFG_FMT_SDBOOT;
    while (s < end) {
        const char *line = s, *key_end, *val;
        size_t vlen;

        while (s < end && *s != '\n')
            s++;
        key_end = line;
        while (key_end < s && *key_end != ' ' && *key_end != '\t')
            key_end++;
        val = key_end;
        while (val < s && (*val == ' ' || *val == '\t'))
            val++;
        vlen = (size_t)(s - val);
        while (vlen > 0 && (val[vlen - 1u] == '\r' || val[vlen - 1u] == ' '))
            vlen--;
        if (s < end)
            s++;
        if (line[0] == '#' || key_end == line)
            continue;
        {
            char key[16];
            char v[BOOTCFG_CMDLINE_MAX];
            size_t kl = (size_t)(key_end - line);

            if (kl >= sizeof key)
                continue;
            memcpy(key, line, kl);
            key[kl] = '\0';
            v[0] = '\0';
            app(v, sizeof v, val, vlen, &e->truncated);
            if (ieq(key, "title")) {
                cpy(e->title, sizeof e->title, v, &e->truncated);
            } else if (ieq(key, "linux")) {
                clean_path(v, e->kernel, sizeof e->kernel, &e->truncated);
            } else if (ieq(key, "initrd")) {
                if (e->n_initrd < BOOTCFG_INITRD_MAX) {
                    clean_path(v, e->initrd[e->n_initrd],
                               sizeof e->initrd[0], &e->truncated);
                    e->n_initrd++;
                } else {
                    e->truncated = 1;
                }
            } else if (ieq(key, "options")) {
                if (e->cmdline[0] != '\0')
                    app(e->cmdline, sizeof e->cmdline, " ", 1, &e->truncated);
                app(e->cmdline, sizeof e->cmdline, v, slen(v), &e->truncated);
            }
        }
    }
}

static void sd_read(parser *p)
{
    size_t i, j;

    if (p->src->list == (int (*)(void *, const char *,
                                 int (*)(void *, const char *), void *))0)
        return;
    g_sd.n = 0;
    if (p->src->list(p->src->ctx, "/loader/entries", sd_collect, &g_sd) != 0)
        return;
    /* Name order, which is the order systemd-boot sorts by when no entry
     * carries a sort-key, and the one archiso's names are written for. */
    for (i = 1; i < g_sd.n; i++) {
        for (j = i; j > 0 && name_lt(g_sd.names[j], g_sd.names[j - 1u]); j--) {
            char t[BOOTCFG_TITLE_MAX];

            memcpy(t, g_sd.names[j], sizeof t);
            memcpy(g_sd.names[j], g_sd.names[j - 1u], sizeof t);
            memcpy(g_sd.names[j - 1u], t, sizeof t);
        }
    }
    for (i = 0; i < g_sd.n; i++) {
        char path[BOOTCFG_PATH_MAX];
        char *buf;
        size_t len = 0;

        path[0] = '\0';
        cpy(path, sizeof path, "/loader/entries/", (uint8_t *)0);
        app(path, sizeof path, g_sd.names[i], slen(g_sd.names[i]),
            (uint8_t *)0);
        buf = file_load(p, path, &len);
        if (buf == (char *)0)
            continue;
        sd_parse(p, buf, len);
        file_release(p, buf, len);
    }
}

/* ------------------------------------------------------------- isolinux */

/* A path from an isolinux config: absolute stays, relative is against the
 * config's directory. */
static void iso_path_join(parser *p, const char *in, size_t n, char *out,
                          size_t cap, uint8_t *trunc)
{
    out[0] = '\0';
    if (n > 0 && in[0] != '/') {
        app(out, cap, p->dir, slen(p->dir), trunc);
        app(out, cap, "/", 1, trunc);
    }
    app(out, cap, in, n, trunc);
    squash_slashes(out);
}

static void isolinux_parse(parser *p, const char *buf, size_t len,
                           unsigned depth);

static void isolinux_include(parser *p, const char *arg, size_t n,
                             unsigned depth)
{
    char path[BOOTCFG_PATH_MAX];
    char *buf;
    size_t len = 0;

    if (depth + 1u >= DEPTH_MAX)
        return;
    iso_path_join(p, arg, n, path, sizeof path, (uint8_t *)0);
    buf = file_load(p, path, &len);
    if (buf == (char *)0)
        return;
    isolinux_parse(p, buf, len, depth + 1u);
    file_release(p, buf, len);
}

/* APPEND's initrd= goes to the entry's initrd list; the rest is cmdline. */
static void isolinux_append(parser *p, bootcfg_entry *e, const char *v,
                            size_t n)
{
    size_t i = 0;

    e->cmdline[0] = '\0';
    while (i < n) {
        size_t start, wl;

        while (i < n && (v[i] == ' ' || v[i] == '\t'))
            i++;
        start = i;
        while (i < n && v[i] != ' ' && v[i] != '\t')
            i++;
        wl = i - start;
        if (wl == 0)
            break;
        if (wl > 7 && lower(v[start]) == 'i' && lower(v[start + 1u]) == 'n' &&
            lower(v[start + 2u]) == 'i' && lower(v[start + 3u]) == 't' &&
            lower(v[start + 4u]) == 'r' && lower(v[start + 5u]) == 'd' &&
            v[start + 6u] == '=') {
            size_t a = start + 7u;

            e->n_initrd = 0;
            while (a < i && e->n_initrd < BOOTCFG_INITRD_MAX) {
                size_t b = a;

                while (b < i && v[b] != ',')
                    b++;
                iso_path_join(p, v + a, b - a, e->initrd[e->n_initrd],
                              sizeof e->initrd[0], &e->truncated);
                e->n_initrd++;
                a = b + 1u;
            }
            continue;
        }
        if (e->cmdline[0] != '\0')
            app(e->cmdline, sizeof e->cmdline, " ", 1, &e->truncated);
        app(e->cmdline, sizeof e->cmdline, v + start, wl, &e->truncated);
    }
}

static void isolinux_parse(parser *p, const char *buf, size_t len,
                           unsigned depth)
{
    const char *s = buf, *end = buf + len;

    while (s < end) {
        const char *line = s, *kw, *kw_end, *val;
        size_t vlen;
        char key[16];
        size_t kl;

        while (s < end && *s != '\n')
            s++;
        kw = line;
        while (kw < s && (*kw == ' ' || *kw == '\t'))
            kw++;
        kw_end = kw;
        while (kw_end < s && *kw_end != ' ' && *kw_end != '\t' &&
               *kw_end != '\r')
            kw_end++;
        val = kw_end;
        while (val < s && (*val == ' ' || *val == '\t'))
            val++;
        vlen = (size_t)(s - val);
        while (vlen > 0 && (val[vlen - 1u] == '\r' || val[vlen - 1u] == ' '))
            vlen--;
        if (s < end)
            s++;
        kl = (size_t)(kw_end - kw);
        if (kl == 0 || kl >= sizeof key || kw[0] == '#')
            continue;
        memcpy(key, kw, kl);
        key[kl] = '\0';

        if (ieq(key, "label")) {
            char t[BOOTCFG_TITLE_MAX];

            t[0] = '\0';
            app(t, sizeof t, val, vlen, (uint8_t *)0);
            p->cur = entry_new(p, t);
            if (p->cur != (bootcfg_entry *)0)
                p->cur->format = BOOTCFG_FMT_ISOLINUX;
        } else if (ieq(key, "include") || ieq(key, "config")) {
            size_t n = 0;

            while (n < vlen && val[n] != ' ')
                n++;
            p->cur = (bootcfg_entry *)0;
            isolinux_include(p, val, n, depth);
        } else if (p->cur != (bootcfg_entry *)0) {
            bootcfg_entry *e = p->cur;

            if (ieq(key, "kernel") || ieq(key, "linux")) {
                size_t n = 0;

                while (n < vlen && val[n] != ' ')
                    n++;
                iso_path_join(p, val, n, e->kernel, sizeof e->kernel,
                              &e->truncated);
            } else if (ieq(key, "append")) {
                isolinux_append(p, e, val, vlen);
            } else if (ieq(key, "initrd")) {
                e->n_initrd = 0;
                iso_path_join(p, val, vlen, e->initrd[0], sizeof e->initrd[0],
                              &e->truncated);
                e->n_initrd = 1;
            }
        }
    }
}

/* -------------------------------------------------------------- policy */

/* Could this entry be what a boot of the ISO means? A kernel, and not one
 * of the tools install media carries beside the OS: memory testers,
 * hardware detection, a chain to another loader. */
static int entry_bootable(const bootcfg_entry *e)
{
    size_t n = slen(e->kernel);

    if (e->kernel[0] == '\0')
        return 0;
    if (contains_ci(e->kernel, "memtest") || contains_ci(e->title, "memtest"))
        return 0;
    if (n > 4 && ieq(e->kernel + n - 4, ".c32"))
        return 0;
    if (contains_ci(e->kernel, "hdt") && contains_ci(e->title, "hardware"))
        return 0;
    return 1;
}

int bootcfg_entry_bootable(const bootcfg_entry *e)
{
    return e != (const bootcfg_entry *)0 && entry_bootable(e);
}

int bootcfg_takes_iso_path(const bootcfg_entry *e)
{
    return e != (const bootcfg_entry *)0 &&
           contains_ci(e->cmdline, BOOTCFG_ISO_TOKEN);
}

/* The first entry from index `from` that the kind accepts. */
static int entry_present(const parser *p, const bootcfg_entry *e)
{
    uint8_t i;

    if (p->src->exists == (int (*)(void *, const char *))0)
        return 1;
    if (!p->src->exists(p->src->ctx, e->kernel))
        return 0;
    for (i = 0; i < e->n_initrd; i++) {
        if (!p->src->exists(p->src->ctx, e->initrd[i]))
            return 0;
    }
    return 1;
}

static size_t pick_from(const parser *p, const bootcfg_result *r, size_t from,
                        bootcfg_kind kind)
{
    size_t i;

    for (i = from; i < r->n; i++) {
        const bootcfg_entry *e = &r->entry[i];

        if (e->nested || e->kernel_unresolved || !entry_bootable(e) ||
            !entry_present(p, e))
            continue;
        /* A cmdline built from a value GRUB computes at run time is not
         * one this module can hand anyone. */
        if (e->unresolved)
            continue;
        if (kind == BOOTCFG_LOOP && !bootcfg_takes_iso_path(e))
            continue;
        return i;
    }
    return BOOTCFG_ENTRIES_MAX;
}

static void parse_grub_file(parser *p, const char *path, int loop)
{
    char *buf;
    size_t len = 0;
    const char *slash;

    buf = file_load(p, path, &len);
    if (buf == (char *)0)
        return;
    /* GRUB's $prefix is the directory its config lives in; distros source
     * files relative to it ("source ${prefix}/grub.cfg"). */
    slash = path + slen(path);
    while (slash > path && *slash != '/')
        slash--;
    {
        char dir[BOOTCFG_PATH_MAX];
        size_t n = (size_t)(slash - path);

        if (n >= sizeof dir)
            n = sizeof dir - 1u;
        memcpy(dir, path, n);
        dir[n] = '\0';
        var_set(p, "prefix", dir);
    }
    p->loop = loop;
    p->n_blk = 0;
    p->n_conds = 0;
    p->submenus = p->skips = p->loops = 0;
    p->cur = (bootcfg_entry *)0;
    grub_parse(p, buf, len, 0);
    p->cur = (bootcfg_entry *)0;
    file_release(p, buf, len);
    cpy(p->res->source, sizeof p->res->source, path, (uint8_t *)0);
}

static const char *const g_grub_cd[] = {
    "/boot/grub/grub.cfg", "/boot/grub2/grub.cfg", "/EFI/BOOT/grub.cfg",
    "/boot/grub/x86_64-efi/grub.cfg", (const char *)0
};
const char *const *bootcfg_grub_cd_paths(void)
{
    return g_grub_cd;
}

static const char *const g_grub_loop[] = {
    "/boot/grub/loopback.cfg", "/boot/grub2/loopback.cfg", (const char *)0
};
static const char *const g_isolinux[] = {
    "/isolinux/isolinux.cfg", "/boot/isolinux/isolinux.cfg",
    "/syslinux/syslinux.cfg", "/boot/syslinux/syslinux.cfg",
    "/isolinux.cfg", "/syslinux.cfg", (const char *)0
};

static void parser_reset(parser *p, const bootcfg_src *src, char *scratch,
                         size_t cap, bootcfg_result *res)
{
    memset(p, 0, sizeof(*p));
    p->src = src;
    p->scratch = scratch;
    p->cap = cap;
    p->res = res;
    /* What GRUB would have on an x86-64 UEFI machine, for configs that
     * branch on it or build paths from it. */
    var_set(p, "grub_platform", "efi");
    var_set(p, "grub_cpu", "x86_64");
    /* $root names the device GRUB booted from, and every use of it in a
     * distro config is a "($root)" device prefix, which a path here drops
     * anyway. Known, and empty, so it does not mark entries unresolved. */
    var_set(p, "root", "");
}

/* Parses one family of files into the result, keeping what earlier passes
 * found, and returns the pick for kind from what this pass added. */
static size_t pass_grub(parser *p, const char *const *files, int loop,
                        bootcfg_kind kind)
{
    size_t i;

    for (i = 0; files[i] != (const char *)0; i++) {
        size_t before = p->res->n;
        size_t pick;

        parse_grub_file(p, files[i], loop);
        pick = pick_from(p, p->res, before, kind);
        if (pick != BOOTCFG_ENTRIES_MAX)
            return pick;
    }
    return BOOTCFG_ENTRIES_MAX;
}

int bootcfg_read(const bootcfg_src *src, bootcfg_kind kind, char *scratch,
                 size_t scratch_len, bootcfg_result *res)
{
    parser *p = &g_p;
    size_t pick = BOOTCFG_ENTRIES_MAX;
    size_t before, i;

    if (src == (const bootcfg_src *)0 || src->read == (int (*)(void *,
            const char *, char *, size_t, size_t *))0 ||
        scratch == (char *)0 || scratch_len < 2 || res == (bootcfg_result *)0)
        return BOOTCFG_ERR_BAD_ARG;
    memset(res, 0, sizeof(*res));
    res->pick = BOOTCFG_ENTRIES_MAX;
    parser_reset(p, src, scratch, scratch_len, res);

    /* The file-boot convention first when that is what is wanted; then the
     * CD's own configuration, which serves both kinds (Debian and Mint put
     * ${iso_path} in their main grub.cfg entries). */
    if (kind == BOOTCFG_LOOP) {
        p->iso_set = 1;
        pick = pass_grub(p, g_grub_loop, 1, kind);
    }
    if (pick == BOOTCFG_ENTRIES_MAX)
        pick = pass_grub(p, g_grub_cd, 0, kind);

    if (pick == BOOTCFG_ENTRIES_MAX) {
        before = res->n;
        sd_read(p);
        pick = pick_from(p, res, before, kind);
        if (pick != BOOTCFG_ENTRIES_MAX)
            cpy(res->source, sizeof res->source, "/loader/entries",
                (uint8_t *)0);
    }
    for (i = 0; pick == BOOTCFG_ENTRIES_MAX && g_isolinux[i] != (const char *)0;
         i++) {
        const char *path = g_isolinux[i];
        const char *slash = path + slen(path);
        char *buf;
        size_t len = 0, n;

        while (slash > path && *slash != '/')
            slash--;
        n = (size_t)(slash - path);
        if (n >= sizeof p->dir)
            n = sizeof p->dir - 1u;
        memcpy(p->dir, path, n);
        p->dir[n] = '\0';
        buf = file_load(p, path, &len);
        if (buf == (char *)0)
            continue;
        before = res->n;
        p->cur = (bootcfg_entry *)0;
        isolinux_parse(p, buf, len, 0);
        p->cur = (bootcfg_entry *)0;
        file_release(p, buf, len);
        pick = pick_from(p, res, before, kind);
        if (pick != BOOTCFG_ENTRIES_MAX)
            cpy(res->source, sizeof res->source, path, (uint8_t *)0);
    }

    /* Last: a medium whose only GRUB config is its loopback.cfg
     * (SystemRescue's CD boot sources the same menu from a config embedded
     * in its EFI binary) is read the way GRUB reads it from a CD, with
     * ${iso_path} empty. */
    if (pick == BOOTCFG_ENTRIES_MAX && kind == BOOTCFG_CD) {
        p->iso_set = 0;
        pick = pass_grub(p, g_grub_loop, 1, kind);
    }

    res->pick = pick;
    if (pick == BOOTCFG_ENTRIES_MAX)
        return p->nospace ? BOOTCFG_ERR_NOSPACE : BOOTCFG_ERR_NONE;
    return BOOTCFG_OK;
}

/* ---------------------------------------------------------------- render */

int bootcfg_render(const bootcfg_entry *e, const char *iso_path, char *out,
                   size_t cap)
{
    const char *s;
    size_t o = 0;
    const size_t tl = slen(BOOTCFG_ISO_TOKEN);

    if (e == (const bootcfg_entry *)0 || out == (char *)0 || cap == 0)
        return BOOTCFG_ERR_BAD_ARG;
    s = e->cmdline;
    while (*s != '\0') {
        const char *w = s;
        size_t wl = 0;
        char word[BOOTCFG_CMDLINE_MAX];
        size_t n = 0, k;
        int had_token = 0;

        while (w[wl] != '\0' && w[wl] != ' ')
            wl++;
        /* The word with the token substituted. */
        for (k = 0; k < wl;) {
            if (k + tl <= wl && memcmp(w + k, BOOTCFG_ISO_TOKEN, tl) == 0) {
                had_token = 1;
                if (iso_path != (const char *)0) {
                    size_t pl = slen(iso_path), j;

                    for (j = 0; j < pl && n + 1u < sizeof word; j++)
                        word[n++] = iso_path[j];
                }
                k += tl;
                continue;
            }
            if (n + 1u < sizeof word)
                word[n++] = w[k];
            k++;
        }
        word[n] = '\0';
        s = w + wl;
        while (*s == ' ')
            s++;
        /* An argument whose whole value was the token, and which is now
         * empty, is left out: GRUB would have passed "findiso=" and the
         * initramfs would treat it as unset, but saying nothing is
         * clearer. Quotes around the value go with it. */
        if (had_token && iso_path == (const char *)0) {
            size_t q = n;

            while (q > 0 && word[q - 1u] == '"')
                q--;
            if (q > 0 && word[q - 1u] == '=')
                continue;
        }
        if (n == 0)
            continue;
        if (o > 0) {
            if (o + 1u >= cap)
                return BOOTCFG_ERR_NOSPACE;
            out[o++] = ' ';
        }
        if (o + n >= cap)
            return BOOTCFG_ERR_NOSPACE;
        memcpy(out + o, word, n);
        o += n;
    }
    out[o] = '\0';
    return BOOTCFG_OK;
}
