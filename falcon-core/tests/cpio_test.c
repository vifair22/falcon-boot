/* Tests for falcon-core/src/media/cpio.c, the newc writer that builds the
 * initrd segment the injection path appends (docs/DESIGN.md Section 6.2).
 *
 * The archive this module produces is parsed by the kernel's own unpacker at
 * boot, with no chance to report a problem: a record whose padding is one
 * byte out does not fail loudly, it makes the rest of the archive unparseable
 * and the appended files silently absent. So the central test here is a
 * byte-for-byte comparison against a golden archive spelled out in full
 * below, field by field, rather than a set of properties that a subtly wrong
 * writer could still satisfy.
 *
 * The golden bytes are hand-constructed here and NOT generated at test time.
 * CI must not need GNU cpio installed to know whether our writer is correct,
 * and a golden buffer that is produced by running the thing it is meant to
 * check would prove nothing anyway.
 *
 * They were cross-checked once, by hand, against GNU cpio 2.15 running
 * "cpio -o -H newc" over the same three entries. The two agree on every byte
 * except the fields we deliberately fix and the trailing block padding:
 *
 *   ino        ours counts from 1; GNU writes the real inode numbers
 *   uid, gid   ours are 0 (root); GNU writes the invoking user's
 *   mtime      ours is 0, so the output is reproducible
 *   devminor   ours is 0; GNU writes the source filesystem's
 *   tail       GNU pads the file out to a 512-byte block, we stop at the
 *              trailer (falcon-core/src/media/cpio.c says why)
 *
 * Everything structural matched exactly: the magic, the field order and
 * widths, uppercase hex, mode values, nlink 2 for the directory and 1 for the
 * files, namesize including the NUL, the header+name padding to 4, the data
 * padding to 4, and the TRAILER!!! record. An optional round-trip test at the
 * bottom re-proves the same thing against whatever cpio is installed, and
 * skips cleanly when there is none.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <media/cpio.h>

#define HDR_LEN 110u
#define BUF_MAX 8192u

/* Sentinel for "the writer must not have touched this byte". */
#define FILLER 0xAB

/* ------------------------------------------------------------- golden */

/* Three entries and the trailer. Every NUL is its own string-literal token so
 * that an escape can never swallow the digit that follows it.
 *
 * Field order, per record (110 bytes, all ASCII hex, uppercase):
 *   magic ino mode uid gid nlink mtime filesize
 *   devmajor devminor rdevmajor rdevminor namesize check */
static const char golden[] =
    /* --- entry 1: directory "dev", mode 040755 --------------------- */
    "070701"      /* magic                                            */
    "00000001"    /* ino 1: inodes count from 1                       */
    "000041ED"    /* mode 040755                                      */
    "00000000"    /* uid root                                         */
    "00000000"    /* gid root                                         */
    "00000002"    /* nlink 2: the convention for a directory          */
    "00000000"    /* mtime fixed at 0                                 */
    "00000000"    /* filesize: a directory has none                   */
    "00000000"    /* devmajor                                         */
    "00000000"    /* devminor                                         */
    "00000000"    /* rdevmajor                                        */
    "00000000"    /* rdevminor                                        */
    "00000004"    /* namesize 4: "dev" plus its NUL                   */
    "00000000"    /* check: unused in newc                            */
    "dev" "\0"
    "\0" "\0"     /* 110 + 4 = 114, padded to 116                     */

    /* --- entry 2: file "dev/x", 3 bytes ---------------------------- */
    "070701"
    "00000002"    /* ino 2                                            */
    "000081A4"    /* mode 0100644                                     */
    "00000000"
    "00000000"
    "00000001"    /* nlink 1: >= 2 would mean a hard link to the
                     kernel's unpacker, which defers the contents     */
    "00000000"
    "00000003"    /* filesize 3                                       */
    "00000000"
    "00000000"
    "00000000"
    "00000000"
    "00000006"    /* namesize 6                                       */
    "00000000"
    "dev/x" "\0"  /* 110 + 6 = 116, already aligned: no padding       */
    "hi\n" "\0"   /* 3 bytes of data, padded to 4                     */

    /* --- entry 3: empty file "e" ----------------------------------- */
    "070701"
    "00000003"    /* ino 3                                            */
    "000081A4"
    "00000000"
    "00000000"
    "00000001"
    "00000000"
    "00000000"    /* filesize 0: an empty file still gets a record    */
    "00000000"
    "00000000"
    "00000000"
    "00000000"
    "00000002"    /* namesize 2                                       */
    "00000000"
    "e" "\0"      /* 110 + 2 = 112, already aligned                   */

    /* --- trailer --------------------------------------------------- */
    "070701"
    "00000000"    /* ino 0                                            */
    "00000000"    /* mode 0                                           */
    "00000000"
    "00000000"
    "00000001"    /* nlink 1                                          */
    "00000000"
    "00000000"
    "00000000"
    "00000000"
    "00000000"
    "00000000"
    "0000000B"    /* namesize 11: "TRAILER!!!" plus its NUL           */
    "00000000"
    "TRAILER!!!" "\0"
    "\0" "\0" "\0"; /* 110 + 11 = 121, padded to 124                  */

/* sizeof counts the literal's own terminator, which is not part of the
 * archive. 116 + 120 + 112 + 124. */
#define GOLDEN_LEN (sizeof(golden) - 1u)

static const char golden_body[] = "hi\n";

static const cpio_entry golden_entries[] = {
    { "dev",   NULL,        0, CPIO_MODE_DIR  },
    { "dev/x", golden_body, 3, CPIO_MODE_FILE },
    { "e",     NULL,        0, CPIO_MODE_FILE },
};
#define GOLDEN_N (sizeof(golden_entries) / sizeof(golden_entries[0]))

/* -------------------------------------------------------------- helpers */

static uint32_t hex8(const uint8_t *p)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < 8; i++) {
        uint8_t c = p[i];

        /* Uppercase only: the writer promises it, so a lowercase digit here
         * is a failure and not something to be tolerant of. */
        assert_true((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    return v;
}

static size_t pad4(size_t n)
{
    return (4u - (n % 4u)) % 4u;
}

/* Walks the emitted archive and asserts the structural invariants nothing
 * else checks: every header on a 4-byte boundary, the magic in place, names
 * NUL-terminated with no embedded NUL, every pad byte actually zero, inodes
 * sequential from 1, nlink matching the file type, mtime zero, and the whole
 * thing ending on the trailer exactly at out_size. Returns the record count,
 * trailer excluded. */
static unsigned check_structure(const uint8_t *buf, size_t len)
{
    size_t pos = 0;
    unsigned n = 0;

    for (;;) {
        uint32_t filesize;
        uint32_t namesize;
        const char *name;
        size_t after_name;
        size_t i;

        /* The invariant the kernel's parser depends on. */
        assert_int_equal(pos % 4u, 0);
        assert_true(pos + HDR_LEN <= len);
        assert_memory_equal(buf + pos, "070701", 6);

        filesize = hex8(buf + pos + 54u);
        namesize = hex8(buf + pos + 94u);
        assert_int_equal(hex8(buf + pos + 22u), 0);  /* uid */
        assert_int_equal(hex8(buf + pos + 30u), 0);  /* gid */
        assert_int_equal(hex8(buf + pos + 46u), 0);  /* mtime */
        assert_int_equal(hex8(buf + pos + 62u), 0);  /* devmajor */
        assert_int_equal(hex8(buf + pos + 70u), 0);  /* devminor */
        assert_int_equal(hex8(buf + pos + 78u), 0);  /* rdevmajor */
        assert_int_equal(hex8(buf + pos + 86u), 0);  /* rdevminor */
        assert_int_equal(hex8(buf + pos + 102u), 0); /* check */

        assert_true(namesize >= 2u);
        assert_true(pos + HDR_LEN + namesize <= len);
        name = (const char *)(buf + pos + HDR_LEN);
        assert_int_equal(name[namesize - 1u], '\0');
        for (i = 0; i + 1u < namesize; i++)
            assert_true(name[i] != '\0');
        assert_true(name[0] != '/');

        after_name = pos + HDR_LEN + namesize;
        for (i = 0; i < pad4(HDR_LEN + namesize); i++)
            assert_int_equal(buf[after_name + i], 0);
        after_name += pad4(HDR_LEN + namesize);

        if (strcmp(name, "TRAILER!!!") == 0) {
            assert_int_equal(filesize, 0);
            assert_int_equal(hex8(buf + pos + 6u), 0);   /* ino */
            assert_int_equal(hex8(buf + pos + 14u), 0);  /* mode */
            assert_int_equal(hex8(buf + pos + 38u), 1);  /* nlink */
            /* Nothing follows the trailer: no block padding. */
            assert_int_equal(after_name, len);
            return n;
        }

        n++;
        assert_int_equal(hex8(buf + pos + 6u), n);
        if ((hex8(buf + pos + 14u) & 0170000u) == 0040000u) {
            assert_int_equal(filesize, 0);
            assert_int_equal(hex8(buf + pos + 38u), 2);
        } else {
            assert_int_equal(hex8(buf + pos + 38u), 1);
        }

        assert_true(after_name + filesize <= len);
        for (i = 0; i < pad4(filesize); i++)
            assert_int_equal(buf[after_name + filesize + i], 0);
        pos = after_name + filesize + pad4(filesize);
    }
}

/* cpio_size must agree with cpio_write to the byte, for whatever shape it is
 * handed: they are the same record-builder run twice, and this is the
 * assertion that keeps them that way. */
static size_t check_size_matches_write(const cpio_entry *ents, size_t count)
{
    uint8_t buf[BUF_MAX];
    size_t sized = 0;
    size_t written = 0;
    size_t exact = 0;

    assert_int_equal(cpio_size(ents, count, &sized), CPIO_OK);
    assert_true(sized <= BUF_MAX);

    memset(buf, FILLER, sizeof(buf));
    assert_int_equal(cpio_write(ents, count, buf, sizeof(buf), &written),
                     CPIO_OK);
    assert_int_equal(written, sized);
    /* Not one byte past what it promised. */
    if (written < BUF_MAX)
        assert_int_equal(buf[written], FILLER);
    assert_int_equal(check_structure(buf, written), count);

    /* An exactly-sized buffer is the normal call, and must not be a
     * boundary failure. */
    memset(buf, FILLER, sizeof(buf));
    assert_int_equal(cpio_write(ents, count, buf, sized, &exact), CPIO_OK);
    assert_int_equal(exact, sized);
    return sized;
}

/* ------------------------------------------------------------- golden */

static void test_cpio_golden_bytes(void **state)
{
    uint8_t buf[BUF_MAX];
    size_t size = 0;
    size_t written = 0;

    (void)state;
    /* If this trips, the hand-built literal above is miscounted, not the
     * writer; checked first so the failure says which. */
    assert_int_equal(GOLDEN_LEN, 472);

    assert_int_equal(cpio_size(golden_entries, GOLDEN_N, &size), CPIO_OK);
    assert_int_equal(size, GOLDEN_LEN);

    memset(buf, FILLER, sizeof(buf));
    assert_int_equal(cpio_write(golden_entries, GOLDEN_N, buf, sizeof(buf),
                                &written), CPIO_OK);
    assert_int_equal(written, GOLDEN_LEN);
    assert_memory_equal(buf, golden, GOLDEN_LEN);
    assert_int_equal(buf[GOLDEN_LEN], FILLER);
}

static void test_cpio_golden_structure(void **state)
{
    uint8_t buf[BUF_MAX];
    size_t written = 0;

    (void)state;
    memset(buf, FILLER, sizeof(buf));
    assert_int_equal(cpio_write(golden_entries, GOLDEN_N, buf, sizeof(buf),
                                &written), CPIO_OK);
    assert_int_equal(check_structure(buf, written), GOLDEN_N);
}

/* An archive of nothing at all is still a valid archive: just the trailer. */
static void test_cpio_empty_archive(void **state)
{
    uint8_t buf[BUF_MAX];
    size_t size = 0;

    (void)state;
    assert_int_equal(cpio_size(NULL, 0, &size), CPIO_OK);
    assert_int_equal(size, 124);
    assert_int_equal(check_size_matches_write(NULL, 0), 124);

    memset(buf, FILLER, sizeof(buf));
    assert_int_equal(cpio_write(NULL, 0, buf, sizeof(buf), &size), CPIO_OK);
    assert_memory_equal(buf, golden + GOLDEN_LEN - 124u, 124);
}

/* ---------------------------------------------------------- size shapes */

/* Name lengths 1..8 put header+name at every residue mod 4, which is the
 * only thing that exercises each branch of the name padding. */
static void test_cpio_size_name_padding(void **state)
{
    static const char *const names[] = {
        "a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg", "abcdefgh",
    };
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        cpio_entry e = { NULL, NULL, 0, CPIO_MODE_FILE };
        size_t name_len = strlen(names[i]);
        size_t want;

        e.path = names[i];
        /* One record plus the trailer, each padded independently. */
        want = HDR_LEN + name_len + 1u;
        want += pad4(want);
        want += 124u;
        assert_int_equal(check_size_matches_write(&e, 1), want);
    }
}

/* Data lengths 0..5 do the same for the data padding. */
static void test_cpio_size_data_padding(void **state)
{
    static const uint8_t body[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t len;

    (void)state;
    for (len = 0; len <= 5; len++) {
        cpio_entry e = { "f", NULL, 0, CPIO_MODE_FILE };
        size_t want;

        e.data = len != 0 ? body : NULL;
        e.len = len;
        want = HDR_LEN + 2u;         /* "f" + NUL, already a multiple of 4 */
        want += len + pad4(len);
        want += 124u;
        assert_int_equal(check_size_matches_write(&e, 1), want);
    }
}

/* The shape the injection path actually emits: a directory tree with hook
 * scripts in it. */
static void test_cpio_size_nested_tree(void **state)
{
    static const char hook[] = "#!/bin/sh\nexit 0\n";
    static const char table[] = "0 2048\n2048 4096\n";
    const cpio_entry ents[] = {
        { "hooks",                   NULL,  0,                CPIO_MODE_DIR  },
        { "hooks/local-top",         NULL,  0,                CPIO_MODE_DIR  },
        { "hooks/local-top/falcon",  hook,  sizeof(hook) - 1, CPIO_MODE_FILE },
        { "falcon",                  NULL,  0,                CPIO_MODE_DIR  },
        { "falcon/extents",          table, sizeof(table) - 1, CPIO_MODE_FILE},
    };

    (void)state;
    assert_true(check_size_matches_write(ents, 5) > 5u * HDR_LEN);
}

/* A file whose mode is left at zero is a regular file, not a mode-0 one. */
static void test_cpio_default_mode(void **state)
{
    cpio_entry e = { "f", NULL, 0, 0 };
    uint8_t buf[BUF_MAX];
    size_t written = 0;

    (void)state;
    memset(buf, FILLER, sizeof(buf));
    assert_int_equal(cpio_write(&e, 1, buf, sizeof(buf), &written), CPIO_OK);
    assert_memory_equal(buf + 14u, "000081A4", 8);
    assert_int_equal(check_structure(buf, written), 1);
}

/* ------------------------------------------------------------- capacity */

static void test_cpio_nospace_writes_nothing(void **state)
{
    uint8_t buf[BUF_MAX];
    size_t need = 0;
    size_t got;
    size_t cap;

    (void)state;
    assert_int_equal(cpio_size(golden_entries, GOLDEN_N, &need), CPIO_OK);

    /* One byte short, and every smaller capacity down to zero, all behave the
     * same way: the required size is reported and the buffer is untouched. A
     * partially written archive would be worse than none, because the kernel
     * would parse the prefix and stop. */
    for (cap = 0; cap < need; cap += (cap < 8u ? 1u : 97u)) {
        memset(buf, FILLER, sizeof(buf));
        got = 0;
        assert_int_equal(cpio_write(golden_entries, GOLDEN_N, buf, cap, &got),
                         CPIO_ERR_NOSPACE);
        assert_int_equal(got, need);
        for (got = 0; got < sizeof(buf); got++)
            assert_int_equal(buf[got], FILLER);
    }

    /* Exactly enough is not NOSPACE. */
    memset(buf, FILLER, sizeof(buf));
    got = 0;
    assert_int_equal(cpio_write(golden_entries, GOLDEN_N, buf, need, &got),
                     CPIO_OK);
    assert_int_equal(got, need);
}

/* ------------------------------------------------------------- bad args */

static void test_cpio_bad_args(void **state)
{
    uint8_t buf[BUF_MAX];
    size_t out = 0;

    (void)state;
    /* Missing out_size: there is nowhere to report the answer. */
    assert_int_equal(cpio_size(golden_entries, GOLDEN_N, NULL),
                     CPIO_ERR_BAD_ARG);
    assert_int_equal(cpio_write(golden_entries, GOLDEN_N, buf, sizeof(buf),
                                NULL), CPIO_ERR_BAD_ARG);
    /* A count with no entries. */
    assert_int_equal(cpio_size(NULL, 1, &out), CPIO_ERR_BAD_ARG);
    assert_int_equal(cpio_write(NULL, 1, buf, sizeof(buf), &out),
                     CPIO_ERR_BAD_ARG);
    /* No buffer to write into. */
    assert_int_equal(cpio_write(golden_entries, GOLDEN_N, NULL, sizeof(buf),
                                &out), CPIO_ERR_BAD_ARG);
}

static void test_cpio_bad_entries(void **state)
{
    static const uint8_t body[4] = { 1, 2, 3, 4 };
    static const struct {
        const char *why;
        cpio_entry  e;
    } cases[] = {
        { "no path at all",
          { NULL, NULL, 0, CPIO_MODE_FILE } },
        { "empty path",
          { "", NULL, 0, CPIO_MODE_FILE } },
        /* An absolute name would unpack outside the initramfs root the
         * archive is meant to describe. */
        { "leading slash",
          { "/etc/passwd", NULL, 0, CPIO_MODE_FILE } },
        { "leading slash on a bare name",
          { "/f", NULL, 0, CPIO_MODE_FILE } },
        /* So would a ".." component: the kernel's unpacker resolves it, so
         * every spelling of it has to be refused and not just the obvious
         * leading one. */
        { "dotdot alone",
          { "..", NULL, 0, CPIO_MODE_FILE } },
        { "dotdot leading",
          { "../foo", NULL, 0, CPIO_MODE_FILE } },
        { "dotdot in the middle",
          { "x/../y", NULL, 0, CPIO_MODE_FILE } },
        { "dotdot trailing",
          { "x/..", NULL, 0, CPIO_MODE_FILE } },
        { "dotdot twice over",
          { "../../etc/passwd", NULL, 0, CPIO_MODE_FILE } },
        { "dotdot as a directory entry",
          { "x/..", NULL, 0, CPIO_MODE_DIR } },
        /* An empty component names nothing, and is what a path built by
         * concatenating onto a directory that already ended in a slash looks
         * like. */
        { "empty component",
          { "a//b", NULL, 0, CPIO_MODE_FILE } },
        { "empty component leading a path",
          { "a///b", NULL, 0, CPIO_MODE_FILE } },
        { "trailing slash on a file",
          { "f/", NULL, 0, CPIO_MODE_FILE } },
        /* Rejected for a directory too: newc names directories without one,
         * and GNU cpio emits none. */
        { "trailing slash on a directory",
          { "d/", NULL, 0, CPIO_MODE_DIR } },
        { "trailing slash after a nested name",
          { "a/b/", NULL, 0, CPIO_MODE_FILE } },
        /* A directory record carries no data; newc has no way to express
         * one, so this is a caller mistake rather than something to drop. */
        { "directory with data",
          { "d", body, 4, CPIO_MODE_DIR } },
        { "length without data",
          { "f", NULL, 4, CPIO_MODE_FILE } },
    };
    uint8_t buf[BUF_MAX];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t out = 12345;
        size_t j;

        assert_int_equal(cpio_size(&cases[i].e, 1, &out), CPIO_ERR_BAD_ARG);

        /* And the write form must reject it before touching the buffer: the
         * validation happens in the sizing pass, so a bad entry anywhere in
         * the list costs the caller nothing. */
        memset(buf, FILLER, sizeof(buf));
        assert_int_equal(cpio_write(&cases[i].e, 1, buf, sizeof(buf), &out),
                         CPIO_ERR_BAD_ARG);
        for (j = 0; j < 256u; j++)
            assert_int_equal(buf[j], FILLER);
    }
}

/* A bad entry in the middle of an otherwise valid list still rejects the
 * whole archive, and still writes nothing. */
static void test_cpio_bad_entry_mid_list(void **state)
{
    const cpio_entry ents[] = {
        { "a",  NULL, 0, CPIO_MODE_FILE },
        { "/b", NULL, 0, CPIO_MODE_FILE },
        { "c",  NULL, 0, CPIO_MODE_FILE },
    };
    uint8_t buf[BUF_MAX];
    size_t out = 0;
    size_t i;

    (void)state;
    memset(buf, FILLER, sizeof(buf));
    assert_int_equal(cpio_write(ents, 3, buf, sizeof(buf), &out),
                     CPIO_ERR_BAD_ARG);
    for (i = 0; i < 256u; i++)
        assert_int_equal(buf[i], FILLER);
}

/* The other side of the path check: names that look like the rejected ones to
 * a careless matcher but are ordinary names, and the "." component, which is
 * legal because it resolves to the directory it already sits in and so cannot
 * leave the archive root. Each must not merely be accepted; the name has to
 * reach the record unaltered, since the writer never rewrites a path and a
 * "helpful" normalisation would be its own bug. */
static void test_cpio_legal_near_misses(void **state)
{
    static const char *const names[] = {
        "..foo",     /* a name that starts with two dots is still a name */
        "a..b",      /* and so is one with two dots inside it            */
        "foo..",     /* or ending in them                                */
        "...",       /* three dots is a name; two would not be           */
        ".hidden",   /* the ordinary dotfile case                        */
        ".",         /* the current directory, spelled as one component  */
        "./x",       /* a leading "." component                          */
        "a/./b",     /* and one in the middle                            */
        "a/.b/c",    /* a dotfile as a directory component               */
    };
    uint8_t buf[BUF_MAX];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        cpio_entry e = { NULL, NULL, 0, CPIO_MODE_FILE };
        size_t written = 0;
        size_t name_len = strlen(names[i]);

        e.path = names[i];
        assert_int_equal(check_size_matches_write(&e, 1),
                         HDR_LEN + name_len + 1u +
                         pad4(HDR_LEN + name_len + 1u) + 124u);

        memset(buf, FILLER, sizeof(buf));
        assert_int_equal(cpio_write(&e, 1, buf, sizeof(buf), &written),
                         CPIO_OK);
        assert_memory_equal(buf + HDR_LEN, names[i], name_len + 1u);
    }
}

/* newc stores the file size in eight hex digits, so anything that does not
 * fit is refused rather than truncated into a plausible-looking record. The
 * data pointer is never dereferenced on this path, which is what makes the
 * test possible without allocating four gigabytes. */
static void test_cpio_field_range(void **state)
{
    (void)state;
#if SIZE_MAX > 0xFFFFFFFFu
    {
        static const uint8_t body[1] = { 0 };
        cpio_entry e = { "f", body, (size_t)0x100000000ull, CPIO_MODE_FILE };
        uint8_t buf[BUF_MAX];
        size_t out = 0;
        size_t i;

        assert_int_equal(cpio_size(&e, 1, &out), CPIO_ERR_RANGE);
        memset(buf, FILLER, sizeof(buf));
        assert_int_equal(cpio_write(&e, 1, buf, sizeof(buf), &out),
                         CPIO_ERR_RANGE);
        for (i = 0; i < 256u; i++)
            assert_int_equal(buf[i], FILLER);
    }
#else
    print_message("  size_t is 32-bit: the newc field range cannot be "
                  "exceeded, nothing to check\n");
#endif
}

/* ---------------------------------------------------- optional round-trip */

static int have_cpio(void)
{
    return system("command -v cpio >/dev/null 2>&1") == 0;
}

static int rm_rf(const char *path)
{
    char cmd[512];

    /* The path is a mkdtemp result, never caller input. */
    if (snprintf(cmd, sizeof(cmd), "rm -rf -- '%s'", path) >= (int)sizeof(cmd))
        return -1;
    return system(cmd);
}

static void write_all(const char *path, const void *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const uint8_t *p = buf;

    assert_true(fd >= 0);
    while (len > 0) {
        ssize_t n = write(fd, p, len);

        assert_true(n > 0);
        p += n;
        len -= (size_t)n;
    }
    assert_int_equal(close(fd), 0);
}

/* Extracts our archive with the real tool and checks what lands on disk. This
 * is the end the kernel is at: an archive that our own parser is happy with
 * but that a real unpacker mis-reads would pass every test above. Skipped,
 * not failed, when cpio is not installed, because CI must not depend on it. */
static void test_cpio_roundtrip_optional(void **state)
{
    char dir[] = "/tmp/falcon-cpio-XXXXXX";
    char arc[320];
    char root[320];
    char path[384];
    uint8_t buf[BUF_MAX];
    size_t written = 0;
    struct stat st;
    pid_t pid;
    int status = -1;
    int fd;
    char got[8];
    ssize_t n;

    (void)state;
    if (!have_cpio()) {
        print_message("  cpio(1) not installed: round-trip skipped\n");
        skip();
    }

    assert_int_equal(cpio_write(golden_entries, GOLDEN_N, buf, sizeof(buf),
                                &written), CPIO_OK);

    assert_true(mkdtemp(dir) != NULL);
    assert_true(snprintf(arc, sizeof(arc), "%s/a.cpio", dir) > 0);
    assert_true(snprintf(root, sizeof(root), "%s/root", dir) > 0);
    assert_int_equal(mkdir(root, 0700), 0);
    write_all(arc, buf, written);

    pid = fork();
    assert_true(pid >= 0);
    if (pid == 0) {
        int in = open(arc, O_RDONLY);

        if (in < 0 || chdir(root) != 0 || dup2(in, STDIN_FILENO) < 0)
            _exit(127);
        execlp("cpio", "cpio", "-i", "-d", "-m", "--quiet", (char *)NULL);
        _exit(127);
    }
    assert_true(waitpid(pid, &status, 0) == pid);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);

    /* The directory record has to have produced a directory... */
    assert_true(snprintf(path, sizeof(path), "%s/dev", root) > 0);
    assert_int_equal(stat(path, &st), 0);
    assert_true(S_ISDIR(st.st_mode));

    /* ...the file inside it its exact bytes... */
    assert_true(snprintf(path, sizeof(path), "%s/dev/x", root) > 0);
    assert_int_equal(stat(path, &st), 0);
    assert_true(S_ISREG(st.st_mode));
    assert_int_equal(st.st_size, 3);
    fd = open(path, O_RDONLY);
    assert_true(fd >= 0);
    n = read(fd, got, sizeof(got));
    assert_int_equal(close(fd), 0);
    assert_int_equal(n, 3);
    assert_memory_equal(got, "hi\n", 3);

    /* ...and the empty file must exist rather than be optimised away. */
    assert_true(snprintf(path, sizeof(path), "%s/e", root) > 0);
    assert_int_equal(stat(path, &st), 0);
    assert_true(S_ISREG(st.st_mode));
    assert_int_equal(st.st_size, 0);

    (void)rm_rf(dir);
}

/* ------------------------------------------------------------------ run */

int run_cpio_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cpio_golden_bytes),
        cmocka_unit_test(test_cpio_golden_structure),
        cmocka_unit_test(test_cpio_empty_archive),
        cmocka_unit_test(test_cpio_size_name_padding),
        cmocka_unit_test(test_cpio_size_data_padding),
        cmocka_unit_test(test_cpio_size_nested_tree),
        cmocka_unit_test(test_cpio_default_mode),
        cmocka_unit_test(test_cpio_nospace_writes_nothing),
        cmocka_unit_test(test_cpio_bad_args),
        cmocka_unit_test(test_cpio_bad_entries),
        cmocka_unit_test(test_cpio_bad_entry_mid_list),
        cmocka_unit_test(test_cpio_legal_near_misses),
        cmocka_unit_test(test_cpio_field_range),
        cmocka_unit_test(test_cpio_roundtrip_optional),
    };

    return cmocka_run_group_tests_name("cpio newc writer", tests, NULL, NULL);
}
