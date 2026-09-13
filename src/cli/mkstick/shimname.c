#include <cli/mkstick/shimname.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <cli/common/log.h>
#include <cli/common/util.h>

/* A signed shim is around one megabyte; the cap is generous enough that a
 * fatter future one still parses and small enough that pointing this at the
 * wrong file cannot read a DVD image into memory. */
#define SHIM_MAX_BYTES ((size_t)32 * 1024 * 1024)

#define MAX_CANDIDATES 8u

typedef struct {
    char   name[MAX_CANDIDATES][SHIM_NEXT_STAGE_MAX];
    size_t count;
    int    overflow;
} candidates;

static void candidate_add(candidates *c, const char *name)
{
    size_t i;

    for (i = 0; i < c->count; i++) {
        if (strcmp(c->name[i], name) == 0)
            return;                       /* the same string appears twice */
    }
    if (c->count >= MAX_CANDIDATES) {
        c->overflow = 1;
        return;
    }
    (void)snprintf(c->name[c->count], SHIM_NEXT_STAGE_MAX, "%s", name);
    c->count++;
}

/* A shim-legal EFI basename: what a FAT path component can hold, ending in
 * .efi, with no separator of either flavour inside it. */
static int is_efi_basename(const char *s)
{
    size_t len = strlen(s);
    size_t i;

    if (len < 5u || len >= SHIM_NEXT_STAGE_MAX)
        return 0;
    for (i = 0; i < len; i++) {
        char ch = s[i];

        if (isalnum((unsigned char)ch) != 0)
            continue;
        if (ch == '.' || ch == '_' || ch == '-' || ch == '+')
            continue;
        return 0;
    }
    return strcasecmp(s + len - 4u, ".efi") == 0;
}

/* The names shim uses for the OTHER two binaries it can launch. They are
 * stored with a single backslash and so never match the next-stage rule; the
 * list exists to keep them out of the single-backslash fallback. */
static int is_helper_name(const char *s)
{
    static const char *const helpers[] = {
        "mmx64.efi", "MokManager.efi", "fbx64.efi", "fallback.efi",
        "shimx64.efi", "shim.efi"
    };
    size_t i;

    for (i = 0; i < sizeof helpers / sizeof helpers[0]; i++) {
        if (strcasecmp(s, helpers[i]) == 0)
            return 1;
    }
    return 0;
}

/* Walks one UTF-16LE, NUL-terminated, printable-ASCII string starting at
 * `pos`. Returns its byte length including the terminator, or 0 when there is
 * no such string here.
 *
 * A run longer than `cap` is consumed and reported as the empty string rather
 * than rejected: it cannot be an EFI path (no FAT name is 255 characters of
 * anything we care about), and skipping the whole run keeps the caller from
 * rescanning a long ASCII blob from every offset inside it. `out` is written
 * on every path, so the caller never inspects uninitialised storage. */
static size_t utf16_run(const unsigned char *data, size_t len, size_t pos,
                        char *out, size_t cap)
{
    size_t i = pos;
    size_t n = 0;
    int too_long = 0;

    out[0] = '\0';
    while (i + 1u < len && data[i + 1u] == 0 &&
           data[i] >= 0x20u && data[i] < 0x7fu) {
        if (n + 1u < cap)
            out[n] = (char)data[i];
        else
            too_long = 1;
        n++;
        i += 2u;
    }
    if (n == 0)
        return 0;
    if (i + 1u >= len || data[i] != 0 || data[i + 1u] != 0)
        return 0;                       /* not NUL-terminated: not a string */
    if (!too_long)
        out[n] = '\0';
    return (i + 2u) - pos;
}

typedef void (*string_visit_fn)(const char *text, void *ctx);

static cli_status scan_utf16_strings(const char *path, string_visit_fn visit,
                                     void *ctx)
{
    void *buf = NULL;
    size_t len = 0;
    const unsigned char *data;
    char text[256];
    cli_status rc;
    size_t i;

    rc = util_read_file(path, &buf, &len, SHIM_MAX_BYTES);
    if (rc != CLI_OK)
        return rc;
    data = (const unsigned char *)buf;

    /* Declared once and cleared once, outside the loop: a buffer that is
     * only partly rewritten per run is fine for the reader and awkward for a
     * static analyser, and clearing a quarter of a kilobyte per FILE costs
     * nothing. */
    memset(text, 0, sizeof text);

    for (i = 0; i + 1u < len;) {
        size_t step;

        if (data[i + 1u] != 0 || data[i] < 0x20u || data[i] >= 0x7fu) {
            i++;
            continue;
        }
        step = utf16_run(data, len, i, text, sizeof text);
        if (step == 0) {
            i += 2u;
            continue;
        }
        i += step;
        visit(text, ctx);
    }

    free(buf);
    return CLI_OK;
}

typedef struct {
    candidates primary;    /* two leading backslashes: the default loader */
    candidates fallback;   /* one backslash, not a name shim reserves */
    candidates mokmanager; /* one backslash, and a name shim reserves for MM */
} shim_strings;

static void visit(const char *text, void *ctx)
{
    shim_strings *s = (shim_strings *)ctx;

    if (text[0] != '\\')
        return;

    /* Primary rule: exactly two leading backslashes, then a basename. Shim's
     * build passes -DDEFAULT_LOADER='L"\\\\grubx64.efi"', so the doubled
     * separator is the signature of the compiled-in next stage and of
     * nothing else in the binary. */
    if (text[1] == '\\' && text[2] != '\\') {
        if (is_efi_basename(text + 2))
            candidate_add(&s->primary, text + 2);
        return;
    }
    if (text[1] == '\\')
        return;

    if (!is_efi_basename(text + 1))
        return;
    if (strcasecmp(text + 1, "mmx64.efi") == 0 ||
        strcasecmp(text + 1, "MokManager.efi") == 0)
        candidate_add(&s->mokmanager, text + 1);
    else if (!is_helper_name(text + 1))
        candidate_add(&s->fallback, text + 1);
}

cli_status shim_next_stage_name(const char *shim_path, char *out, size_t cap)
{
    shim_strings s;
    cli_status rc;
    size_t i;

    memset(&s, 0, sizeof s);
    rc = scan_utf16_strings(shim_path, visit, &s);
    if (rc != CLI_OK)
        return rc;

    if (s.primary.count == 1u) {
        if (snprintf(out, cap, "%s", s.primary.name[0]) < 0)
            return set_error(CLI_ERR_INTERNAL, "formatting the shim name");
        log_debug("%s: next-stage default is \\\\%s", shim_path, out);
        return CLI_OK;
    }

    if (s.primary.count > 1u) {
        for (i = 0; i < s.primary.count; i++)
            log_error("  candidate: \\\\%s", s.primary.name[i]);
        return set_error(CLI_ERR_INVALID,
                         "%s carries %zu next-stage candidates; refusing to "
                         "guess which name our loader must take",
                         shim_path, s.primary.count);
    }

    if (s.fallback.count == 1u) {
        log_warn("%s has no doubled-backslash default loader string; falling "
                 "back to the single-backslash candidate", shim_path);
        if (snprintf(out, cap, "%s", s.fallback.name[0]) < 0)
            return set_error(CLI_ERR_INTERNAL, "formatting the shim name");
        return CLI_OK;
    }

    for (i = 0; i < s.fallback.count; i++)
        log_error("  candidate: \\%s", s.fallback.name[i]);
    return set_error(CLI_ERR_INVALID,
                     "could not read a next-stage loader name out of %s "
                     "(%zu ambiguous candidates); the shim layout this tool "
                     "was measured against has changed, see "
                     "docs/notes/shim-spike.md",
                     shim_path, s.fallback.count);
}

cli_status shim_mokmanager_name(const char *shim_path, char *out, size_t cap)
{
    shim_strings s;
    cli_status rc;
    size_t i;

    memset(&s, 0, sizeof s);
    rc = scan_utf16_strings(shim_path, visit, &s);
    if (rc != CLI_OK)
        return rc;

    if (s.mokmanager.count == 1u) {
        if (snprintf(out, cap, "%s", s.mokmanager.name[0]) < 0)
            return set_error(CLI_ERR_INTERNAL, "formatting the MM name");
        return CLI_OK;
    }
    if (s.mokmanager.count > 1u) {
        for (i = 0; i < s.mokmanager.count; i++)
            log_error("  candidate: \\%s", s.mokmanager.name[i]);
        return set_error(CLI_ERR_INVALID,
                         "%s names %zu MokManager binaries", shim_path,
                         s.mokmanager.count);
    }

    log_warn("%s names no MokManager binary; installing it as mmx64.efi",
             shim_path);
    if (snprintf(out, cap, "%s", "mmx64.efi") < 0)
        return set_error(CLI_ERR_INTERNAL, "formatting the MM name");
    return CLI_OK;
}
