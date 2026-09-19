/* The grub.cfg served in place of an ISO's own. See grubcfg.h. */

#include <media/grubcfg.h>

#include <lib/string.h>

typedef struct {
    char  *p;
    size_t cap;
    size_t n;
    int    over;
} wbuf;

static void put(wbuf *w, const char *s, size_t len)
{
    if (w->over || len > w->cap - w->n) {
        w->over = 1;
        return;
    }
    memcpy(w->p + w->n, s, len);
    w->n += len;
}

static void puts_(wbuf *w, const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    put(w, s, n);
}

static void put_dec(wbuf *w, uint64_t v)
{
    char d[24];
    size_t at = sizeof d;

    /* Filled from the end, so the digits come out in order. */
    do {
        d[--at] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0);
    put(w, d + at, sizeof d - at);
}

/* Characters GRUB takes literally outside quotes. Anything else ($, ;, {,
 * spaces, quotes) makes the word single-quoted, which GRUB never expands. */
static int plain(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
           c == ',' || c == ':' || c == '=' || c == '+' || c == '-' ||
           c == '@';
}

/* One word, quoted when it needs to be. A ' inside becomes '\'' . */
static void put_word(wbuf *w, const char *s, size_t len)
{
    size_t i;
    int quote = 0;

    for (i = 0; i < len; i++)
        if (!plain(s[i]))
            quote = 1;
    if (!quote) {
        put(w, s, len);
        return;
    }
    put(w, "'", 1);
    for (i = 0; i < len; i++) {
        if (s[i] == '\'')
            put(w, "'\\''", 4);
        else
            put(w, s + i, 1);
    }
    put(w, "'", 1);
}

/* The cmdline, word by word. */
static void put_cmdline(wbuf *w, const char *s)
{
    size_t i = 0;

    for (;;) {
        size_t start;

        while (s[i] == ' ' || s[i] == '\t')
            i++;
        if (s[i] == '\0')
            return;
        start = i;
        while (s[i] != '\0' && s[i] != ' ' && s[i] != '\t')
            i++;
        put(w, " ", 1);
        put_word(w, s + start, i - start);
    }
}

static void put_path(wbuf *w, const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    put_word(w, s, n);
}

int grubcfg_write(const bootcfg_entry *e, const char *cmdline,
                  uint64_t hook_sector, uint64_t hook_count, size_t size,
                  char *out, size_t cap)
{
    wbuf w;
    size_t i;

    if (e == (const bootcfg_entry *)0 || cmdline == (const char *)0 ||
        out == (char *)0 || cap < size || size == 0 || hook_count == 0 ||
        e->kernel[0] == '\0')
        return GRUBCFG_ERR_ARG;

    w.p = out;
    w.cap = size;
    w.n = 0;
    w.over = 0;

    puts_(&w, "# served by falcon-boot in place of this ISO's grub.cfg\n"
              "set timeout=0\nset default=0\n"
              "menuentry 'falcon-boot' {\n\tlinux ");
    put_path(&w, e->kernel);
    put_cmdline(&w, cmdline);
    puts_(&w, "\n\tinitrd");
    for (i = 0; i < e->n_initrd && i < BOOTCFG_INITRD_MAX; i++) {
        put(&w, " ", 1);
        put_path(&w, e->initrd[i]);
    }
    /* A blocklist: 512-byte sectors of the device GRUB booted from, which
     * is the virtual CD. */
    puts_(&w, " ($root)");
    put_dec(&w, hook_sector);
    put(&w, "+", 1);
    put_dec(&w, hook_count);
    puts_(&w, "\n}\n");

    /* The rest of the file's length: a comment GRUB skips, ending in a
     * newline. */
    if (w.over || w.n + 2u > size)
        return GRUBCFG_ERR_SPACE;
    out[w.n++] = '#';
    while (w.n < size - 1u)
        out[w.n++] = ' ';
    out[w.n++] = '\n';
    return GRUBCFG_OK;
}
