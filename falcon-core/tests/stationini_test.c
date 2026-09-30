/* Tests for falcon-core/src/media/stationini.c: STATION.INI, the station's
 * configuration (docs/DESIGN.md Section 13.7).
 *
 * The parser is strict because a typo must not become a different default:
 * every error below has to leave no default and no countdown, and name the
 * line it found. The writer is the installer's, so everything it writes has
 * to parse back to what it was given.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <cmocka.h>

#include <string.h>

#include <media/stationini.h>

/* The header's own example. */
static const char k_example[] =
    "[general]\n"
    "default = win2k          ; a section name, or an autodetected entry's\n"
    "timeout = 5              ; seconds; 0 or absent: the menu waits\n"
    "order   = win2k, linux   ; names, first shown first; the rest follow\n"
    "\n"
    "[win2k]                  ; one section per entry\n"
    "disk  = mbr:cdcccdcc     ; or gpt:<DiskGUID>\n"
    "part  = 1                ; absent: the disk's own MBR\n"
    "label = Windows 2000\n"
    "swap  = no               ; BIOS chain of another disk: no drive swap\n";

static stationini g_cfg;
static stationini g_back;

static int parse(const char *s)
{
    return stationini_parse(s, strlen(s), &g_cfg);
}

/* An error: the line, a sentence containing `frag`, and no default or
 * countdown left behind. */
static void expect_err(const char *s, unsigned line, const char *frag)
{
    assert_int_equal(parse(s), -1);
    assert_int_equal(g_cfg.err_line, line);
    if (strstr(g_cfg.why, frag) == NULL)
        fail_msg("why '%s' lacks '%s'", g_cfg.why, frag);
    assert_string_equal(g_cfg.default_name, "");
    assert_int_equal(g_cfg.timeout, 0);
    assert_false(g_cfg.poweroff_set);
}

static void test_example(void **state)
{
    const stationini_entry *e;

    (void)state;
    assert_int_equal(parse(k_example), 0);
    assert_int_equal(g_cfg.err_line, 0);
    assert_string_equal(g_cfg.why, "");
    assert_string_equal(g_cfg.default_name, "win2k");
    assert_int_equal(g_cfg.timeout, 5);
    assert_int_equal(g_cfg.n_order, 2);
    assert_string_equal(g_cfg.order[0], "win2k");
    assert_string_equal(g_cfg.order[1], "linux");
    assert_int_equal(g_cfg.n_entries, 1);
    e = stationini_find(&g_cfg, "win2k");
    assert_non_null(e);
    assert_int_equal(e->line, 6);
    assert_true(e->has_disk);
    assert_int_equal(e->disk.kind, PART_KIND_MBR);
    assert_int_equal(e->disk.mbr_sig, 0xcdcccdccu);
    assert_int_equal(e->part, 1);
    assert_string_equal(e->label, "Windows 2000");
    assert_int_equal(e->swap, STATIONINI_TRI_NO);
    assert_int_equal(e->hide, STATIONINI_TRI_UNSET);
    assert_int_equal(e->method, STATIONINI_METHOD_UNSET);
    assert_string_equal(e->uki, "");
    assert_string_equal(e->efi, "");
}

static void test_every_key(void **state)
{
    const stationini_entry *e;
    static const uint8_t guid[16] = {
        0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };

    (void)state;
    assert_int_equal(parse(
        "[win11]\n"
        "disk = gpt:00112233-4455-6677-8899-AABBCCDDEEFF\n"
        "efi = \\EFI\\Microsoft\\Boot\\bootmgfw.efi\n"
        "method = efi\n"
        "hide = no\n"
        "[arch]\n"
        "uki = arch-linux.efi\n"
        "hide = yes\n"
        "[fw]\n"
        "disk = mbr:00000001\n"
        "method = firmware\n"
        "swap = yes\n"), 0);
    assert_int_equal(g_cfg.n_entries, 3);
    assert_string_equal(g_cfg.default_name, "");
    assert_int_equal(g_cfg.timeout, 0);
    assert_int_equal(g_cfg.n_order, 0);

    e = stationini_find(&g_cfg, "win11");
    assert_non_null(e);
    assert_int_equal(e->disk.kind, PART_KIND_GPT);
    assert_memory_equal(e->disk.guid, guid, 16);
    assert_int_equal(e->part, 0);
    assert_string_equal(e->efi, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
    assert_int_equal(e->method, STATIONINI_METHOD_EFI);
    assert_int_equal(e->hide, STATIONINI_TRI_NO);

    e = stationini_find(&g_cfg, "arch");
    assert_non_null(e);
    assert_false(e->has_disk);
    assert_string_equal(e->uki, "arch-linux.efi");
    assert_int_equal(e->hide, STATIONINI_TRI_YES);
    assert_int_equal(e->line, 6);

    e = stationini_find(&g_cfg, "fw");
    assert_non_null(e);
    assert_int_equal(e->method, STATIONINI_METHOD_FIRMWARE);
    assert_int_equal(e->swap, STATIONINI_TRI_YES);
    assert_int_equal(e->disk.mbr_sig, 1);
}

static void test_case_comments_crlf(void **state)
{
    const stationini_entry *e;

    (void)state;
    assert_int_equal(parse(
        "# leading comment\r\n"
        "\r\n"
        "  [ GENERAL ]  # the general section\r\n"
        "DEFAULT = Linux_1\r\n"
        "TimeOut=10\r\n"
        "\t; indented comment\r\n"
        "[Linux_1]\r\n"
        "UKI=a.efi;trailing\r\n"
        "HIDE = YES\r\n"
        "Swap = No\r\n"
        "Method = EFI\r\n"
        "LABEL = Arch Linux   \r\n"
        "[x]"), 0);
    assert_string_equal(g_cfg.default_name, "Linux_1");
    assert_int_equal(g_cfg.timeout, 10);
    e = stationini_find(&g_cfg, "Linux_1");
    assert_non_null(e);
    assert_string_equal(e->uki, "a.efi");
    assert_int_equal(e->hide, STATIONINI_TRI_YES);
    assert_int_equal(e->swap, STATIONINI_TRI_NO);
    assert_int_equal(e->method, STATIONINI_METHOD_EFI);
    assert_string_equal(e->label, "Arch Linux");
    assert_int_equal(e->line, 7);
    /* Names are not matched without regard to case. */
    assert_null(stationini_find(&g_cfg, "linux_1"));
    assert_non_null(stationini_find(&g_cfg, "x"));
}

static void test_empty_and_null(void **state)
{
    (void)state;
    assert_int_equal(stationini_parse("", 0, &g_cfg), 0);
    assert_int_equal(g_cfg.n_entries, 0);
    assert_int_equal(stationini_parse(NULL, 0, &g_cfg), 0);
    assert_int_equal(stationini_parse(NULL, 4, &g_cfg), -1);
    assert_int_equal(stationini_parse("x", 1, NULL), -1);
    assert_int_equal(parse("; only\n\n   \n"), 0);
    /* The length is honoured, not the terminator. */
    assert_int_equal(stationini_parse("[a]\nbogus", 4, &g_cfg), 0);
    assert_int_equal(g_cfg.n_entries, 1);

    assert_null(stationini_find(NULL, "a"));
    assert_null(stationini_find(&g_cfg, NULL));
    assert_null(stationini_find(&g_cfg, "b"));
}

static void test_timeout_and_default(void **state)
{
    (void)state;
    /* A zero timeout waits and needs no default. */
    assert_int_equal(parse("[general]\ntimeout = 0\n"), 0);
    assert_int_equal(g_cfg.timeout, 0);
    assert_int_equal(parse("[general]\ndefault = a\ntimeout = 3600\n[a]\n"), 0);
    assert_int_equal(g_cfg.timeout, 3600);
    /* [general] after the entries is as good as before them. */
    assert_int_equal(parse("[a]\nhide=yes\n[general]\ndefault=a\n"), 0);
    assert_string_equal(g_cfg.default_name, "a");

    expect_err("[general]\ntimeout = 5\n", 2, "default is not");
    expect_err("[general]\ntimeout = 3601\ndefault=memtest\n", 2, "timeout");
    expect_err("[general]\ntimeout = \n", 2, "timeout");
    expect_err("[general]\ntimeout = 5s\n", 2, "3600");
    expect_err("[general]\ndefault = win2k\n", 2, "names no entry");
    expect_err("[general]\ndefault = win 2k\n", 2, "is not a name");
    expect_err("[general]\ndefault =\n", 2, "is not a name");
}

static void test_default_autonames(void **state)
{
    static const char *const ok[] = {
        "memtest", "mbr-cdcccdcc", "mbr-CDCCCDCC-p1", "gpt-0011aabb-p12",
        "gpt-0011aabb", "nxt-0003", "nxt-ABcd", "uki-arch-linux", "uki-x", "pos",
    };
    static const char *const bad[] = {
        "memtes", "mbr-cdcccdc", "mbr-cdcccdcg", "mbr-cdcccdcc-", "mbr-cdcccdcc-p",
        "mbr-cdcccdcc-q1", "mbr-cdcccdcc-p1x", "mbr-cdcccdccx", "gpt-", "nxt-003",
        "nxt-00030", "nxt-000g", "uki-", "foo-bar1", "abcd", "po", "poss",
    };
    char buf[96];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        snprintf(buf, sizeof buf, "[general]\ndefault = %s\ntimeout=5\n", ok[i]);
        if (parse(buf) != 0)
            fail_msg("'%s' refused: %s", ok[i], g_cfg.why);
        assert_string_equal(g_cfg.default_name, ok[i]);
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(buf, sizeof buf, "[general]\ndefault = %s\n", bad[i]);
        if (parse(buf) != -1)
            fail_msg("'%s' accepted", bad[i]);
        assert_int_equal(g_cfg.err_line, 2);
    }
}

static void test_order(void **state)
{
    char buf[2048];
    size_t n;
    unsigned i;

    (void)state;
    assert_int_equal(parse("[general]\norder = a,b ,  c\n"), 0);
    assert_int_equal(g_cfg.n_order, 3);
    assert_string_equal(g_cfg.order[2], "c");
    assert_int_equal(parse("[general]\norder = only\n"), 0);
    assert_int_equal(g_cfg.n_order, 1);

    expect_err("[general]\norder =\n", 2, "order names nothing");
    expect_err("[general]\norder = a,,b\n", 2, "order entry");
    expect_err("[general]\norder = a,\n", 2, "order entry");
    expect_err("[general]\norder = a b\n", 2, "is not a name");
    expect_err("[general]\norder = a, b, a\n", 2, "twice");

    n = (size_t)snprintf(buf, sizeof buf, "[general]\norder = ");
    for (i = 0; i < STATIONINI_ORDER_MAX; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%sn%u", i ? "," : "", i);
    snprintf(buf + n, sizeof buf - n, "\n");
    assert_int_equal(parse(buf), 0);
    assert_int_equal(g_cfg.n_order, STATIONINI_ORDER_MAX);
    snprintf(buf + n, sizeof buf - n, ",extra\n");
    expect_err(buf, 2, "more than 32");
}

static void test_names(void **state)
{
    char buf[128];

    (void)state;
    /* 31 characters is the longest a name can be. */
    assert_int_equal(parse("[abcdefghijABCDEFGHIJ0123456789_]\n"), 0);
    expect_err("[abcdefghijABCDEFGHIJ0123456789_-]\n", 1, "is not a name");
    expect_err("[]\n", 1, "is not a name");
    expect_err("[a.b]\n", 1, "is not a name");
    expect_err("[a\n", 1, "section header");
    expect_err("[a]x\n", 1, "section header");
    expect_err("[\n", 1, "section header");
    snprintf(buf, sizeof buf, "[general]\ndefault = %s\n",
             "abcdefghijABCDEFGHIJ0123456789_x");
    expect_err(buf, 2, "is not a name");
}

static void test_duplicates(void **state)
{
    (void)state;
    expect_err("[a]\n[b]\n[a]\n", 3, "is given twice");
    expect_err("[general]\n[a]\n[General]\n", 3, "[general] is given twice");
    expect_err("[general]\ndefault=memtest\ndefault=memtest\n", 3,
               "default is given twice");
    expect_err("[general]\ntimeout=1\nTIMEOUT=1\n", 3, "timeout is given twice");
    expect_err("[general]\norder=a\norder=b\n", 3, "order is given twice");
    expect_err("[a]\ndisk=mbr:00000001\ndisk=mbr:00000001\n", 3,
               "disk is given twice");
    expect_err("[a]\nhide=yes\nHide=no\n", 3, "hide is given twice");
    /* The same key in two sections is two keys. */
    assert_int_equal(parse("[a]\nhide=yes\n[b]\nhide=yes\n"), 0);
}

static void test_bad_values(void **state)
{
    char buf[512];

    (void)state;
    expect_err("[a]\ndisk = mbr:cdcccdc\n", 2, "disk 'mbr:cdcccdc' is not");
    expect_err("[a]\ndisk = MBR:cdcccdcc\n", 2, "disk");
    expect_err("[a]\ndisk = gpt:00112233-4455-6677-8899-AABBCCDDEEF\n", 2,
               "disk");
    expect_err("[a]\ndisk = gpt:00112233x4455-6677-8899-AABBCCDDEEFF\n", 2,
               "disk");
    expect_err("[a]\ndisk=mbr:00000001\npart = 0\n", 3, "partition number");
    expect_err("[a]\ndisk=mbr:00000001\npart = 10000\n", 3, "part");
    expect_err("[a]\ndisk=mbr:00000001\npart = -1\n", 3, "part");
    expect_err("[a]\nmethod = bios\n", 2, "firmware or efi");
    expect_err("[a]\nswap = true\n", 2, "yes or no");
    expect_err("[a]\nhide = 1\n", 2, "yes or no");
    expect_err("[a]\nuki =\n", 2, "uki");
    expect_err("[a]\nefi =\n", 2, "efi");
    expect_err("[a]\nlabel =\n", 2, "label");
    expect_err("[a]\nlabel = tab\there\n", 2, "label");
    expect_err("[a]\nlabel = hi\x7f\n", 2, "label");
    memset(buf, 0, sizeof buf);
    strcpy(buf, "[a]\nlabel = ");
    memset(buf + strlen(buf), 'x', STATIONINI_LABEL_MAX);
    strcat(buf, "\n");
    expect_err(buf, 2, "63 characters");
    memset(buf, 0, sizeof buf);
    strcpy(buf, "[a]\nuki = ");
    memset(buf + strlen(buf), 'x', STATIONINI_PATH_MAX);
    strcat(buf, "\n");
    expect_err(buf, 2, "127 characters");
    /* The limit itself fits. */
    memset(buf, 0, sizeof buf);
    strcpy(buf, "[a]\nefi = ");
    memset(buf + strlen(buf), 'x', STATIONINI_PATH_MAX - 1u);
    assert_int_equal(parse(buf), 0);
    assert_int_equal(strlen(g_cfg.entries[0].efi), STATIONINI_PATH_MAX - 1u);
    /* A long bad value is cut in the sentence, not overflowing it. */
    memset(buf, 0, sizeof buf);
    strcpy(buf, "[a]\nmethod = ");
    memset(buf + strlen(buf), 'z', 200);
    expect_err(buf, 2, "zzzzzzzzzzzzzzzzzzzzzzzz'");
    assert_true(strlen(g_cfg.why) < STATIONINI_WHY_MAX);
}

static void test_structure_errors(void **state)
{
    (void)state;
    expect_err("hide = yes\n", 1, "outside a section");
    expect_err("[a]\nbogus = 1\n", 2, "unknown key 'bogus'");
    expect_err("[a]\ndefault = a\n", 2, "unknown key");
    expect_err("[general]\nlabel = x\n", 2, "unknown key");
    expect_err("[a]\n= x\n", 2, "unknown key");
    expect_err("[a]\njust words\n", 2, "not key=value");
    expect_err("[a]\npart = 2\n[b]\n", 1, "part is given without disk");
    expect_err("[general]\ndefault=a\n[a]\npart = 2\n", 3, "without disk");
}

static void test_errors_keep_entries(void **state)
{
    (void)state;
    expect_err("[general]\ndefault = a\ntimeout = 5\n[a]\nhide = yes\n"
               "[b]\nhide = maybe\n[c]\n", 7, "yes or no");
    assert_int_equal(g_cfg.n_entries, 2);
    assert_int_equal(g_cfg.entries[0].hide, STATIONINI_TRI_YES);
    assert_string_equal(g_cfg.entries[1].name, "b");
}

static void test_too_many_entries(void **state)
{
    char buf[1024];
    size_t n = 0;
    unsigned i;

    (void)state;
    for (i = 0; i < STATIONINI_ENTRIES_MAX; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "[e%u]\n", i);
    assert_int_equal(parse(buf), 0);
    assert_int_equal(g_cfg.n_entries, STATIONINI_ENTRIES_MAX);
    snprintf(buf + n, sizeof buf - n, "[over]\n");
    expect_err(buf, STATIONINI_ENTRIES_MAX + 1u, "more than 32");
}

/* ------------------------------------------------------------ the writer */

static void round_trip(const char *text)
{
    static char out[16384];
    size_t n;

    assert_int_equal(parse(text), 0);
    n = stationini_format(&g_cfg, out, sizeof out);
    /* An empty configuration is an empty file, which is also how "does not
     * fit" reads; the caller tells them apart by what it passed in. */
    if (n == 0 && (g_cfg.n_entries != 0 || g_cfg.n_order != 0 ||
                   g_cfg.default_name[0] != '\0' || g_cfg.timeout != 0))
        fail_msg("format refused:\n%s", text);
    assert_string_equal(out, n == 0 ? "" : out);
    assert_int_equal(strlen(out), n);
    if (stationini_parse(out, n, &g_back) != 0)
        fail_msg("reparse failed at %u: %s\n%s", g_back.err_line, g_back.why,
                 out);
    /* Only the line numbers may differ. */
    assert_string_equal(g_back.default_name, g_cfg.default_name);
    assert_int_equal(g_back.timeout, g_cfg.timeout);
    assert_int_equal(g_back.n_order, g_cfg.n_order);
    assert_memory_equal(g_back.order, g_cfg.order, sizeof g_cfg.order);
    assert_int_equal(g_back.n_entries, g_cfg.n_entries);
    for (unsigned i = 0; i < g_cfg.n_entries; i++)
        g_back.entries[i].line = g_cfg.entries[i].line;
    assert_memory_equal(g_back.entries, g_cfg.entries, sizeof g_cfg.entries);
    assert_memory_equal(&g_back.totp, &g_cfg.totp, sizeof g_cfg.totp);
}

static void test_round_trip(void **state)
{
    char buf[4096];
    size_t n = 0;
    unsigned i;

    (void)state;
    round_trip(k_example);
    round_trip("");
    round_trip("[general]\ntimeout = 0\n");
    round_trip("[general]\norder = z\n[z]\n");
    round_trip(
        "[general]\ndefault = nxt-0003\ntimeout = 3600\norder = a, b\n"
        "[a]\ndisk = gpt:00112233-4455-6677-8899-AABBCCDDEEFF\npart = 9999\n"
        "efi = \\EFI\\x y\\boot.efi\nmethod = efi\nswap = yes\n"
        "label = A = B (c)\nhide = no\n"
        "[b]\nuki = arch.efi\nhide = yes\n"
        "[c]\ndisk = mbr:00000000\nmethod = firmware\nswap = no\n");

    for (i = 0; i < STATIONINI_ENTRIES_MAX; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n,
                              "[e%u]\ndisk = mbr:%08x\npart = %u\n", i,
                              i * 0x01010101u, i + 1u);
    round_trip(buf);
}

static void test_format_exact(void **state)
{
    char out[1024];

    (void)state;
    assert_int_equal(parse(k_example), 0);
    assert_true(stationini_format(&g_cfg, out, sizeof out) > 0);
    assert_string_equal(out,
        "[general]\n"
        "default = win2k\n"
        "timeout = 5\n"
        "order = win2k, linux\n"
        "\n"
        "[win2k]\n"
        "disk = mbr:cdcccdcc\n"
        "part = 1\n"
        "swap = no\n"
        "label = Windows 2000\n");
    /* No [general] when it would be empty. */
    assert_int_equal(parse("[m]\nhide = yes\n"), 0);
    assert_true(stationini_format(&g_cfg, out, sizeof out) > 0);
    assert_string_equal(out, "[m]\nhide = yes\n");
}

/* The idle power-off: seconds, 0 never, absent not set (the loader's
 * default), and written back only when set. */
static void test_poweroff(void **state)
{
    char out[256];

    (void)state;
    assert_int_equal(parse("[general]\ndefault = memtest\n"), 0);
    assert_false(g_cfg.poweroff_set);
    assert_int_equal(parse("[general]\npoweroff = 0\n"), 0);
    assert_true(g_cfg.poweroff_set);
    assert_int_equal(g_cfg.poweroff, 0);
    assert_int_equal(parse("[general]\npoweroff = 86400\n"), 0);
    assert_int_equal(g_cfg.poweroff, 86400);
    assert_true(stationini_format(&g_cfg, out, sizeof out) > 0);
    assert_string_equal(out, "[general]\npoweroff = 86400\n");

    expect_err("[general]\npoweroff = 86401\n", 2, "up to 86400");
    expect_err("[general]\npoweroff = 5m\n", 2, "poweroff");
    expect_err("[general]\npoweroff = 5\npoweroff = 6\n", 3, "twice");
    expect_err("[m]\npoweroff = 5\n", 2, "unknown key");
}

static void test_format_capacity(void **state)
{
    char out[256];
    size_t full;
    size_t cap;

    (void)state;
    assert_int_equal(parse(k_example), 0);
    full = stationini_format(&g_cfg, out, sizeof out);
    assert_true(full > 0);
    /* Exactly enough fits; one byte less does not, and leaves "". */
    assert_int_equal(stationini_format(&g_cfg, out, full + 1u), full);
    for (cap = 1; cap <= full; cap++) {
        memset(out, 'x', sizeof out);
        assert_int_equal(stationini_format(&g_cfg, out, cap), 0);
        assert_int_equal(out[0], '\0');
    }
    assert_int_equal(stationini_format(&g_cfg, out, 0), 0);
    assert_int_equal(stationini_format(NULL, out, sizeof out), 0);
    assert_int_equal(stationini_format(&g_cfg, NULL, sizeof out), 0);
}

/* A struct the parser could never have produced is refused, not written. */
static void test_format_refusals(void **state)
{
    char out[1024];
    stationini *c = &g_cfg;

    (void)state;
#define BASE() assert_int_equal(parse("[general]\ndefault = a\n" \
        "order = a\n[a]\ndisk = mbr:00000001\npart = 1\n[b]\n"), 0)
#define REFUSED() assert_int_equal(stationini_format(c, out, sizeof out), 0)

    BASE();
    assert_true(stationini_format(c, out, sizeof out) > 0);

    BASE(); c->n_entries = STATIONINI_ENTRIES_MAX + 1u; REFUSED();
    BASE(); c->n_order = STATIONINI_ORDER_MAX + 1u; REFUSED();
    BASE(); c->timeout = STATIONINI_TIMEOUT_MAX + 1u; REFUSED();
    BASE(); strcpy(c->default_name, "a b"); REFUSED();
    BASE(); memset(c->default_name, 'a', sizeof c->default_name); REFUSED();
    BASE(); strcpy(c->default_name, "nothing"); REFUSED();
    BASE(); strcpy(c->default_name, "memtest");
    assert_true(stationini_format(c, out, sizeof out) > 0);
    BASE(); c->default_name[0] = '\0'; c->timeout = 5; REFUSED();
    BASE(); c->default_name[0] = '\0';
    assert_true(stationini_format(c, out, sizeof out) > 0);
    BASE(); strcpy(c->order[0], "x,y"); REFUSED();
    BASE(); c->n_order = 2; strcpy(c->order[1], "a"); REFUSED();
    BASE(); strcpy(c->entries[1].name, "a"); REFUSED();
    BASE(); strcpy(c->entries[1].name, "GENERAL"); REFUSED();
    BASE(); strcpy(c->entries[1].name, ""); REFUSED();
    BASE(); strcpy(c->entries[1].label, "x ; y"); REFUSED();
    BASE(); strcpy(c->entries[1].label, "x # y"); REFUSED();
    BASE(); strcpy(c->entries[1].label, " x"); REFUSED();
    BASE(); strcpy(c->entries[1].label, "x "); REFUSED();
    BASE(); strcpy(c->entries[1].label, "x\ny"); REFUSED();
    BASE(); memset(c->entries[1].label, 'l', sizeof c->entries[1].label);
    REFUSED();
    BASE(); strcpy(c->entries[1].uki, "a\tb"); REFUSED();
    BASE(); strcpy(c->entries[1].efi, "#"); REFUSED();
    BASE(); c->entries[1].swap = (stationini_tri)3; REFUSED();
    BASE(); c->entries[1].hide = (stationini_tri)7; REFUSED();
    BASE(); c->entries[1].method = (stationini_method)3; REFUSED();
    BASE(); c->entries[0].part = 10000; REFUSED();
    BASE(); c->entries[1].part = 2; REFUSED();
    BASE(); c->entries[0].disk.kind = (part_kind)0; REFUSED();
    assert_string_equal(out, "");
#undef BASE
#undef REFUSED
}

/* ------------------------------------------------------------ [totp] */

#define NAME2 "000b00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

static const char k_totp2[] =
    "[general]\ndefault = memtest\n"
    "[TOTP] ; the station's code\n"
    "tpm   = 2.0\n"
    "index = 0x01800FA1\n"
    "pcrs  = 0, 2,4 , 7\n"
    "key   = " NAME2 "\n"
    "label = falcon-station:desk\n"
    "[m]\nhide = yes\n";

/* A 1.2 key: a 2048-bit modulus, 512 hex digits. */
static void key12(char *out, size_t bytes)
{
    size_t i;

    for (i = 0; i < bytes; i++)
        sprintf(out + 2u * i, "%02x", (unsigned)(i & 0xFFu));
}

static void test_totp(void **state)
{
    char buf[2048];
    char key[520];

    (void)state;
    assert_int_equal(parse(k_totp2), 0);
    assert_true(g_cfg.totp.present);
    assert_int_equal(g_cfg.totp.tpm, 20);
    assert_true(g_cfg.totp.index == 0x01800FA1u);
    assert_true(g_cfg.totp.pcrs == 0x95u);
    assert_int_equal(g_cfg.totp.key_len, 34);
    assert_int_equal(g_cfg.totp.key[1], 0x0B);
    assert_int_equal(g_cfg.totp.key[33], 0xFF);
    assert_string_equal(g_cfg.totp.label, "falcon-station:desk");
    assert_int_equal(g_cfg.n_entries, 1);
    round_trip(k_totp2);

    key12(key, 256);
    snprintf(buf, sizeof buf, "[totp]\ntpm = 1.2\nindex = 0x20000fa1\n"
             "pcrs = 15\nkey = %s\n", key);
    assert_int_equal(parse(buf), 0);
    assert_int_equal(g_cfg.totp.tpm, 12);
    assert_int_equal(g_cfg.totp.srk_sha1, 0);
    assert_int_equal(g_cfg.totp.key_len, 256);
    assert_true(g_cfg.totp.pcrs == 0x8000u);
    assert_string_equal(g_cfg.totp.label, "");
    round_trip(buf);
    snprintf(buf, sizeof buf, "[totp]\ntpm = 1.2\nindex = 0x20000fa1\n"
             "pcrs = 15\nkey = %s\nsrkauth = SHA1-zero\n", key);
    assert_int_equal(parse(buf), 0);
    assert_int_equal(g_cfg.totp.srk_sha1, 1);
    round_trip(buf);
    snprintf(buf, sizeof buf, "[totp]\ntpm = 1.2\nindex = 0x20000fa1\n"
             "pcrs = 15\nkey = %s\nsrkauth = zero\n", key);
    assert_int_equal(parse(buf), 0);
    assert_int_equal(g_cfg.totp.srk_sha1, 0);

    /* No section, no TOTP. */
    assert_int_equal(parse(k_example), 0);
    assert_false(g_cfg.totp.present);
}

static void test_totp_errors(void **state)
{
    char buf[2048];
    char key[600];

    (void)state;
    expect_err("[totp]\ntpm = 2\n", 2, "not 2.0 or 1.2");
    expect_err("[totp]\nindex = 1800FA1\n", 2, "index");
    expect_err("[totp]\nindex = 0x\n", 2, "index");
    expect_err("[totp]\nindex = 0x123456789\n", 2, "index");
    expect_err("[totp]\nindex = 0xFG\n", 2, "index");
    expect_err("[totp]\npcrs = 16\n", 2, "PCR numbers");
    expect_err("[totp]\npcrs = 1, 1\n", 2, "PCR numbers");
    expect_err("[totp]\npcrs = 1,\n", 2, "PCR numbers");
    expect_err("[totp]\npcrs =\n", 2, "PCR numbers");
    expect_err("[totp]\nkey = abc\n", 2, "hex");
    expect_err("[totp]\nkey = zz\n", 2, "hex");
    expect_err("[totp]\nlabel = a\tb\n", 2, "label");
    expect_err("[totp]\nhide = yes\n", 2, "unknown key");
    expect_err("[totp]\nsrkauth = one\n", 2, "zero or sha1-zero");
    expect_err("[totp]\ntpm = 2.0\nindex = 0x1\npcrs = 0\nkey = " NAME2
               "\nsrkauth = sha1-zero\n", 1, "for a TPM 1.2");
    expect_err("[totp]\ntpm = 2.0\ntpm = 1.2\n", 3, "twice");
    expect_err("[totp]\ntpm = 2.0\nindex = 0x1\npcrs = 0\nkey = " NAME2
               "\n[totp]\n", 6, "twice");
    expect_err("[general]\n[totp]\ntpm = 2.0\n", 2, "needs tpm");
    expect_err("[totp]\ntpm = 2.0\nindex = 0x1\npcrs = 0\n[x]\n", 1,
               "needs tpm");
    expect_err("[general]\ndefault = memtest\n[totp]\ntpm = 1.2\n"
               "index = 0x1\npcrs = 0\nkey = " NAME2 "\n", 3,
               "not a key");
    /* A 2.0 name whose algorithm is not SHA-256. */
    expect_err("[totp]\ntpm = 2.0\nindex = 0x1\npcrs = 0\nkey = 000c"
               "00112233445566778899aabbccddeeff00112233445566778899aabbccdd"
               "eeff\n", 1, "not a key");
    memset(key, 'a', 514);
    key[514] = '\0';
    snprintf(buf, sizeof buf, "[totp]\nkey = %s\n", key);
    expect_err(buf, 2, "hex of up to 256");
    assert_false(g_cfg.totp.present);
}

static void test_totp_format(void **state)
{
    char out[1024];
    stationini_totp t;

    (void)state;
    assert_int_equal(parse(k_totp2), 0);
    t = g_cfg.totp;
    assert_true(stationini_format_totp(&t, out, sizeof out) > 0);
    assert_string_equal(out, "[totp]\ntpm = 2.0\nindex = 0x01800fa1\n"
                        "pcrs = 0, 2, 4, 7\nkey = " NAME2 "\n"
                        "label = falcon-station:desk\n");
    assert_int_equal(stationini_format_totp(&t, out, 20), 0);
    assert_int_equal(stationini_format_totp(NULL, out, sizeof out), 0);
    assert_int_equal(stationini_format_totp(&t, NULL, sizeof out), 0);
    t.present = 0;
    assert_int_equal(stationini_format_totp(&t, out, sizeof out), 0);
    t = g_cfg.totp;
    t.pcrs = 1u << 16;
    assert_int_equal(stationini_format_totp(&t, out, sizeof out), 0);
    t = g_cfg.totp;
    t.tpm = 3;
    assert_int_equal(stationini_format_totp(&t, out, sizeof out), 0);
    t = g_cfg.totp;
    t.key_len = 20;
    assert_int_equal(stationini_format_totp(&t, out, sizeof out), 0);
    t = g_cfg.totp;
    strcpy(t.label, "a;b");
    assert_int_equal(stationini_format_totp(&t, out, sizeof out), 0);
    t = g_cfg.totp;
    t.srk_sha1 = 1;
    assert_int_equal(stationini_format_totp(&t, out, sizeof out), 0);
    /* The whole file refuses a bad [totp] and an entry named totp. */
    g_cfg.totp.tpm = 3;
    assert_int_equal(stationini_format(&g_cfg, out, sizeof out), 0);
    assert_int_equal(parse(k_totp2), 0);
    strcpy(g_cfg.entries[0].name, "Totp");
    assert_int_equal(stationini_format(&g_cfg, out, sizeof out), 0);
}

static void test_drop_section(void **state)
{
    static const char in[] =
        "# kept\n"
        "[general]\ndefault = memtest ; me too\n"
        "\n"
        "  [ totp ] ; old\n"
        "tpm = 2.0\n"
        "; a comment inside goes with it\n"
        "[win]\nlabel = W\n"
        "[totp]\nkey = 00\n";
    char out[512];
    size_t n;

    (void)state;
    n = stationini_drop_section(in, strlen(in), "totp", out, sizeof out);
    assert_string_equal(out, "# kept\n[general]\ndefault = memtest ; me too"
                        "\n\n[win]\nlabel = W\n");
    assert_int_equal(n, strlen(out));
    /* Nothing to drop: the text itself. A last line with no newline. */
    n = stationini_drop_section("[a]\nx = 1", 9, "totp", out, sizeof out);
    assert_int_equal(n, 9);
    assert_string_equal(out, "[a]\nx = 1");
    n = stationini_drop_section("[totp]\nx = 1", 12, "totp", out,
                                sizeof out);
    assert_int_equal(n, 0);
    assert_string_equal(out, "");
    /* Too small, and arguments. */
    assert_true(stationini_drop_section(in, strlen(in), "totp", out, 10) ==
                (size_t)-1);
    assert_true(stationini_drop_section(NULL, 0, "totp", out, 10) ==
                (size_t)-1);
    assert_true(stationini_drop_section(in, 1, NULL, out, 10) == (size_t)-1);
    assert_true(stationini_drop_section(in, 1, "totp", NULL, 10) ==
                (size_t)-1);
    assert_true(stationini_drop_section(in, 1, "totp", out, 0) ==
                (size_t)-1);
}

int run_stationini_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_example),
        cmocka_unit_test(test_every_key),
        cmocka_unit_test(test_case_comments_crlf),
        cmocka_unit_test(test_empty_and_null),
        cmocka_unit_test(test_timeout_and_default),
        cmocka_unit_test(test_default_autonames),
        cmocka_unit_test(test_poweroff),
        cmocka_unit_test(test_order),
        cmocka_unit_test(test_names),
        cmocka_unit_test(test_duplicates),
        cmocka_unit_test(test_bad_values),
        cmocka_unit_test(test_structure_errors),
        cmocka_unit_test(test_errors_keep_entries),
        cmocka_unit_test(test_too_many_entries),
        cmocka_unit_test(test_round_trip),
        cmocka_unit_test(test_format_exact),
        cmocka_unit_test(test_format_capacity),
        cmocka_unit_test(test_format_refusals),
        cmocka_unit_test(test_totp),
        cmocka_unit_test(test_totp_errors),
        cmocka_unit_test(test_totp_format),
        cmocka_unit_test(test_drop_section),
    };

    return cmocka_run_group_tests_name("stationini", tests, NULL, NULL);
}
