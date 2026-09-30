/* Fixture-backed tests for falcon-core/src/fs/jbd2.c, through the one caller
 * it has: ext4_mount_journal.
 *
 * The images are ext4 filesystems copied while the kernel still had them
 * mounted, and each manifest is what the kernel's own recovery makes of the
 * same copy (falcon-core/tools/mk-test-jbd2.sh). Agreement between the two,
 * every path, type, size and sha256, is the evidence that the replay reads
 * jbd2 as the kernel does. The refusals are driven the way ext4_test.c
 * drives its own: the image is held in memory and patched there, so the
 * walk below it stays genuine.
 *
 * If the images are absent the group is skipped with the command that
 * builds them.
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
#include <unistd.h>

#include <fs/ext4.h>
#include <fs/fs.h>
#include <crypto/sha256.h>

#define PATH_CAP   512
#define MAN_MAX    512
#define WALK_DEPTH 8

typedef struct {
    const char *img;
    const char *man;
    uint8_t *bytes;     /* the whole image, read in once */
    size_t len;
} fixture;

static fixture fx_csum = { "build/test-jbd2-csum.img",
                           "build/test-jbd2-csum.manifest", NULL, 0 };
static fixture fx_nocsum = { "build/test-jbd2-nocsum.img",
                             "build/test-jbd2-nocsum.manifest", NULL, 0 };
static fixture fx_fc = { "build/test-jbd2-fc.img",
                         "build/test-jbd2-fc.manifest", NULL, 0 };

/* ------------------------------------------------------------- plumbing */

/* Reads the in-memory image, patched or not. fail_after counts down the
 * reads left before every read fails, for the I/O sweep; negative is never. */
typedef struct {
    const uint8_t *bytes;
    size_t len;
    long fail_after;
    unsigned long reads;
} mem_src;

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    mem_src *s = ctx;

    s->reads++;
    if (s->fail_after >= 0) {
        if (s->fail_after == 0)
            return -1;
        s->fail_after--;
    }
    if (off > s->len || len > s->len - off)
        return -1;
    memcpy(buf, s->bytes + off, (size_t)len);
    return 0;
}

static int load(fixture *f)
{
    struct stat st;
    int fd = open(f->img, O_RDONLY);
    size_t got = 0;

    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }
    f->len = (size_t)st.st_size;
    f->bytes = malloc(f->len);
    if (f->bytes == NULL) {
        close(fd);
        return -1;
    }
    while (got < f->len) {
        ssize_t n = pread(fd, f->bytes + got, f->len - got, (off_t)got);

        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    return 0;
}

/* The table's memory, from malloc, with every get matched against a put. */
typedef struct {
    unsigned gets;
    unsigned puts;
    size_t last;
    int refuse;
    void *held;
} mem_ledger;

static void *led_get(void *ctx, size_t bytes)
{
    mem_ledger *l = ctx;

    l->last = bytes;
    if (l->refuse)
        return NULL;
    l->gets++;
    l->held = malloc(bytes);
    return l->held;
}

static void led_put(void *ctx, void *mem, size_t bytes)
{
    mem_ledger *l = ctx;

    assert_ptr_equal(mem, l->held);
    assert_int_equal(bytes, l->last);
    l->puts++;
    free(mem);
    l->held = NULL;
}

/* After a successful mount the block is the caller's to release. */
static void led_release(mem_ledger *l)
{
    free(l->held);
    l->held = NULL;
}

/* ------------------------------------------------------------- manifest */

typedef struct {
    char type;
    char path[PATH_CAP];
    uint64_t size;
    char sha[65];
    int seen;
} man_ent;

static man_ent man[MAN_MAX];
static size_t man_n;

static int load_manifest(const char *path)
{
    char line[1024];
    FILE *f = fopen(path, "r");

    if (f == NULL)
        return -1;
    man_n = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char ty[8], p[PATH_CAP], sha[65];
        unsigned long long size = 0;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%7[^\t]\t%511[^\t]\t%llu\t%64s", ty, p, &size,
                   sha) != 4)
            continue;
        if (man_n >= MAN_MAX) {
            fclose(f);
            return -1;
        }
        man[man_n].type = ty[0];
        snprintf(man[man_n].path, PATH_CAP, "%s", p);
        man[man_n].size = size;
        snprintf(man[man_n].sha, sizeof man[0].sha, "%s", sha);
        man[man_n].seen = 0;
        man_n++;
    }
    fclose(f);
    return man_n > 0 ? 0 : -1;
}

static man_ent *man_find(const char *path)
{
    size_t i;

    for (i = 0; i < man_n; i++) {
        if (strcmp(man[i].path, path) == 0)
            return &man[i];
    }
    return NULL;
}

static void file_sha(ext4_vol *v, const ext4_dirent *e, const char *path,
                     char *hex, uint64_t *size)
{
    static uint8_t chunk[64u * 1024u];
    static const char digits[] = "0123456789abcdef";
    ext4_file f;
    sha256_ctx c;
    uint8_t d[SHA256_DIGEST_LEN];
    uint64_t off = 0;
    size_t i;

    assert_int_equal(ext4_file_open(v, &f, e), EXT4_OK);
    *size = ext4_file_size(&f);
    sha256_init(&c);
    while (off < *size) {
        uint64_t take = *size - off;
        int rc;

        if (take > sizeof chunk)
            take = sizeof chunk;
        rc = ext4_file_read(&f, off, take, chunk);
        if (rc != EXT4_OK)
            fail_msg("%s: read at %llu returned %d", path,
                     (unsigned long long)off, rc);
        sha256_update(&c, chunk, (size_t)take);
        off += take;
    }
    sha256_final(&c, d);
    for (i = 0; i < SHA256_DIGEST_LEN; i++) {
        hex[2u * i] = digits[d[i] >> 4];
        hex[2u * i + 1u] = digits[d[i] & 0xFu];
    }
    hex[2u * SHA256_DIGEST_LEN] = '\0';
}

/* Walks the replayed volume and checks every object against the manifest:
 * each one found is in it with the same type, and a file with the same size
 * and hash. With only non-NULL, just that path is checked and the rest is
 * walked for reachability alone. Returns how many objects matched. */
static unsigned walk(ext4_vol *v, ext4_dir *d, const char *prefix,
                     unsigned depth, const char *only)
{
    ext4_dirent e;
    unsigned matched = 0;
    int rc;

    assert_true(depth < WALK_DEPTH);
    while ((rc = ext4_dir_next(d, &e)) == 1) {
        char child[PATH_CAP];
        man_ent *m;
        int check;

        snprintf(child, sizeof child, "%s/%s", prefix, (const char *)e.name);
        check = (only == NULL || strcmp(only, child) == 0);
        m = man_find(child);
        if (check) {
            if (m == NULL)
                fail_msg("%s is in the replay but not the kernel's recovery",
                         child);
            m->seen = 1;
        }
        if (e.is_dir) {
            ext4_dir sub;

            if (check)
                assert_int_equal(m->type, 'd');
            assert_int_equal(ext4_dir_open(v, &sub, &e), EXT4_OK);
            matched += walk(v, &sub, child, depth + 1u, only);
        } else if (e.is_regular && check) {
            char hex[65];
            uint64_t size;

            assert_int_equal(m->type, 'f');
            file_sha(v, &e, child, hex, &size);
            assert_int_equal(size, m->size);
            if (strcmp(hex, m->sha) != 0)
                fail_msg("%s: replay %s, kernel %s", child, hex, m->sha);
        }
        if (check)
            matched++;
    }
    assert_int_equal(rc, 0);
    return matched;
}

static int mount_fixture(fixture *f, ext4_vol *v, mem_src *src,
                         mem_ledger *led, ext4_jinfo *info)
{
    jbd2_mem mem;

    memset(src, 0, sizeof(*src));
    src->bytes = f->bytes;
    src->len = f->len;
    src->fail_after = -1;
    memset(led, 0, sizeof(*led));
    mem.get = led_get;
    mem.put = led_put;
    mem.ctx = led;
    return ext4_mount_journal(v, mem_read, src, &mem, info);
}

/* Journal block location in the in-memory image: the first block at a
 * block-aligned offset carrying the journal magic and type t, with sequence
 * seq (or any, when seq is ~0). */
static long find_jblock(const fixture *f, uint32_t bs, uint32_t t,
                        uint32_t seq, size_t from)
{
    size_t off;

    for (off = from; off + bs <= f->len; off += bs) {
        const uint8_t *b = f->bytes + off;
        uint32_t magic = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                         ((uint32_t)b[2] << 8) | b[3];
        uint32_t type = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                        ((uint32_t)b[6] << 8) | b[7];
        uint32_t sq = ((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) |
                      ((uint32_t)b[10] << 8) | b[11];

        if (magic == 0xC03B3998u && type == t && (seq == ~0u || sq == seq))
            return (long)off;
    }
    return -1;
}

/* The journal's oldest live transaction, from its superblock. */
static uint32_t journal_seq0(const fixture *f, uint32_t bs)
{
    long at = find_jblock(f, bs, 4u, ~0u, 0);
    const uint8_t *s;

    assert_true(at >= 0);
    s = f->bytes + at;
    return ((uint32_t)s[0x18] << 24) | ((uint32_t)s[0x19] << 16) |
           ((uint32_t)s[0x1A] << 8) | s[0x1B];
}

/* ---------------------------------------------------------------- tests */

static void replay_matches(fixture *f)
{
    ext4_vol v;
    ext4_dir root;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    size_t i;

    assert_int_equal(load_manifest(f->man), 0);

    /* The plain mount still refuses: replay is opt-in per call site. */
    memset(&src, 0, sizeof src);
    src.bytes = f->bytes;
    src.len = f->len;
    src.fail_after = -1;
    assert_int_equal(ext4_mount(&v, mem_read, &src), EXT4_ERR_NEEDS_RECOVERY);

    assert_int_equal(mount_fixture(f, &v, &src, &led, &info), EXT4_OK);
    assert_int_equal(info.flags, EXT4_JNL_REPLAYED);
    assert_int_equal(led.gets, 1);
    assert_int_equal(led.puts, 0);
    assert_true(info.need > v.block_size);
    assert_non_null(v.jmap.e);
    assert_true(v.jmap.n > 0);

    assert_int_equal(ext4_dir_open_root(&v, &root), EXT4_OK);
    (void)walk(&v, &root, "", 0, NULL);
    for (i = 0; i < man_n; i++) {
        if (!man[i].seen)
            fail_msg("%s is in the kernel's recovery but not the replay",
                     man[i].path);
    }
    led_release(&led);
}

static void test_jbd2_csum_matches_kernel(void **state)
{
    (void)state;
    replay_matches(&fx_csum);
}

static void test_jbd2_nocsum_matches_kernel(void **state)
{
    (void)state;
    replay_matches(&fx_nocsum);
}

/* The fixtures are only a test of the replay if the replay changes what is
 * read: some copies the table serves must differ from the block at home,
 * or the kernel's recovery and a reader that ignored the journal would
 * agree. The escape case is only covered if an escaped copy is served, and
 * the revoke case only if a revoke block sits in the live log. Each is said
 * here rather than left to pass on a fixture that lost it (a sync in the
 * script, for one, writes everything home and empties the live log). */
static void fixture_is_meaningful(fixture *f, uint32_t bs, int want_escape,
                                  int want_revoke)
{
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    uint32_t seq0 = journal_seq0(f, bs);
    unsigned differ = 0;
    unsigned escaped = 0;
    uint32_t i;
    uint32_t s;
    int revoke = 0;

    assert_int_equal(mount_fixture(f, &v, &src, &led, &info), EXT4_OK);
    for (i = 0; i < v.jmap.n; i++) {
        uint64_t at = (v.jmap.e[i].loc & 0x0000FFFFFFFFFFFFull) * bs;
        uint64_t home = v.jmap.e[i].fsblk * bs;

        /* The first four bytes are skipped: an escaped copy differs there
         * by construction. */
        if (memcmp(f->bytes + at + 4, f->bytes + home + 4, bs - 4u) != 0)
            differ++;
        if ((v.jmap.e[i].loc >> 63) != 0)
            escaped++;
    }
    for (s = 0; s < 64u && !revoke; s++)
        revoke = find_jblock(f, bs, 5u, seq0 + s, 0) >= 0;
    led_release(&led);
    if (differ == 0)
        fail_msg("%s: every journal copy is already home", f->img);
    if (want_escape && escaped == 0)
        fail_msg("%s: no escaped copy is served", f->img);
    if (want_revoke && !revoke)
        fail_msg("%s: no revoke block in the live log", f->img);
}

static void test_jbd2_fixtures_meaningful(void **state)
{
    (void)state;
    fixture_is_meaningful(&fx_csum, 4096u, 1, 0);
    fixture_is_meaningful(&fx_nocsum, 1024u, 0, 1);
}

static void test_jbd2_fast_commit_skipped(void **state)
{
    ext4_vol v;
    ext4_dir root;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;

    (void)state;
    assert_int_equal(load_manifest(fx_fc.man), 0);
    assert_int_equal(mount_fixture(&fx_fc, &v, &src, &led, &info), EXT4_OK);
    assert_int_equal(info.flags, EXT4_JNL_REPLAYED | EXT4_JNL_FC_SKIPPED);
    assert_int_equal(ext4_dir_open_root(&v, &root), EXT4_OK);
    assert_int_equal(walk(&v, &root, "", 0, "/before.txt"), 1);
    led_release(&led);
}

static void test_jbd2_memory(void **state)
{
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    jbd2_mem mem;

    (void)state;
    memset(&src, 0, sizeof src);
    src.bytes = fx_csum.bytes;
    src.len = fx_csum.len;
    src.fail_after = -1;

    /* No provider at all: refused, and what it would have needed said. */
    assert_int_equal(ext4_mount_journal(&v, mem_read, &src, NULL, &info),
                     EXT4_ERR_NOMEM);
    assert_true(info.need > 0);

    /* A provider that has none to give. */
    memset(&led, 0, sizeof led);
    led.refuse = 1;
    mem.get = led_get;
    mem.put = led_put;
    mem.ctx = &led;
    assert_int_equal(ext4_mount_journal(&v, mem_read, &src, &mem, &info),
                     EXT4_ERR_NOMEM);
    assert_int_equal(led.last, info.need);
    assert_int_equal(led.gets, 0);

    assert_int_equal(ext4_mount_journal(&v, mem_read, &src, &mem, NULL),
                     EXT4_ERR_BAD_ARG);

    /* Through the registry, whose FS_ statuses are what the loaders see. */
    {
        fs_vol fv;
        fs_jinfo fi;
        fs_jmem fm = { led_get, led_put, &led };

        assert_int_equal(fs_mount(&fv, &fs_ops_ext4, mem_read, &src),
                         FS_ERR_NEEDS_RECOVERY);
        assert_int_equal(fs_mount_journal(&fv, &fs_ops_ext4, mem_read, &src,
                                          &fm, &fi), FS_ERR_NOMEM);
        assert_true(fi.need > 0);
        led.refuse = 0;
        assert_int_equal(fs_mount_journal(&fv, &fs_ops_ext4, mem_read, &src,
                                          &fm, &fi), FS_OK);
        assert_int_equal(fi.flags, FS_JNL_REPLAYED);
        led_release(&led);
        /* A driver with no journal falls back to its plain mount. */
        assert_int_not_equal(fs_mount_journal(&fv, &fs_ops_fat, mem_read,
                                              &src, &fm, &fi), FS_OK);
        assert_int_equal(fi.flags, 0);
    }
}

/* Every read the mount makes, failed in turn: an error each time, never a
 * crash, and never a block left out. */
static void test_jbd2_io_failure_sweep(void **state)
{
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    unsigned long total;
    unsigned long k;

    (void)state;
    assert_int_equal(mount_fixture(&fx_csum, &v, &src, &led, &info), EXT4_OK);
    total = src.reads;
    led_release(&led);
    assert_true(total > 4);

    for (k = 0; k < total; k++) {
        jbd2_mem mem;
        int rc;

        memset(&src, 0, sizeof src);
        src.bytes = fx_csum.bytes;
        src.len = fx_csum.len;
        src.fail_after = (long)k;
        memset(&led, 0, sizeof led);
        mem.get = led_get;
        mem.put = led_put;
        mem.ctx = &led;
        rc = ext4_mount_journal(&v, mem_read, &src, &mem, &info);
        assert_int_not_equal(rc, EXT4_OK);
        assert_int_equal(led.gets, led.puts);
    }
}

/* A damaged descriptor in a committed transaction is corruption, not the
 * end of the log. */
static void test_jbd2_descriptor_damage(void **state)
{
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    uint32_t seq0 = journal_seq0(&fx_csum, 4096u);
    long at = find_jblock(&fx_csum, 4096u, 1u, seq0, 0);

    (void)state;
    assert_true(at >= 0);
    fx_csum.bytes[at + 20] ^= 0x01u;
    assert_int_equal(mount_fixture(&fx_csum, &v, &src, &led, &info),
                     EXT4_ERR_JOURNAL);
    assert_int_equal(led.gets, 0);
    fx_csum.bytes[at + 20] ^= 0x01u;
}

/* A damaged commit ends the log there: the mount goes on with fewer
 * transactions, as the kernel's does. The byte is h_commit_nsec, inside the
 * header: past it the kernel also accepts a checksum over the header alone
 * with the rest of the block zero (a tail not yet zeroed on disk), so a
 * flip there is rightly still a good commit. */
static void test_jbd2_commit_damage(void **state)
{
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    ext4_jinfo whole;
    uint32_t seq0 = journal_seq0(&fx_csum, 4096u);
    long at;

    (void)state;
    assert_int_equal(mount_fixture(&fx_csum, &v, &src, &led, &whole), EXT4_OK);
    led_release(&led);

    at = find_jblock(&fx_csum, 4096u, 2u, seq0, 0);
    assert_true(at >= 0);
    fx_csum.bytes[at + 0x3A] ^= 0x01u;
    assert_int_equal(mount_fixture(&fx_csum, &v, &src, &led, &info), EXT4_OK);
    /* The log now ends before its first transaction, so nothing is
     * replayed and the ask is the scratch block alone. */
    assert_true(whole.need > 4096u);
    assert_int_equal(info.need, 4096u);
    led_release(&led);
    fx_csum.bytes[at + 0x3A] ^= 0x01u;
}

/* A journal copy whose own tag checksum fails is refused when it is read,
 * which may be at mount (the superblock and descriptors are copies too) or
 * later; either way as EXT4_ERR_JOURNAL. */
static void test_jbd2_copy_damage(void **state)
{
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    uint64_t pblk;
    uint64_t fsblk;
    uint8_t buf[4096];
    int rc;

    (void)state;
    assert_int_equal(mount_fixture(&fx_csum, &v, &src, &led, &info), EXT4_OK);
    pblk = v.jmap.e[v.jmap.n - 1u].loc & 0x0000FFFFFFFFFFFFull;
    fsblk = v.jmap.e[v.jmap.n - 1u].fsblk;
    led_release(&led);

    fx_csum.bytes[pblk * 4096u + 100u] ^= 0x01u;
    rc = mount_fixture(&fx_csum, &v, &src, &led, &info);
    if (rc == EXT4_OK) {
        assert_int_equal(jbd2_read(&v.jmap, fsblk * 4096u, 4096u, buf),
                         JBD2_ERR_CSUM);
        led_release(&led);
    } else {
        assert_int_equal(rc, EXT4_ERR_JOURNAL);
        assert_int_equal(led.gets, led.puts);
    }
    fx_csum.bytes[pblk * 4096u + 100u] ^= 0x01u;
}

/* Bytes flipped across the live log, one at a time: whatever the mount
 * says, it says it without crashing, and a mount that succeeds reads a
 * volume whose walk terminates. */
static void test_jbd2_corruption_sweep(void **state)
{
    fixture *fs[2] = { &fx_csum, &fx_nocsum };
    uint32_t bss[2] = { 4096u, 1024u };
    unsigned which;

    (void)state;
    for (which = 0; which < 2; which++) {
        fixture *f = fs[which];
        uint32_t bs = bss[which];
        uint32_t seq0 = journal_seq0(f, bs);
        long first = find_jblock(f, bs, 1u, seq0, 0);
        static const uint32_t offs[] = { 3, 7, 11, 12, 15, 17, 23, 29,
                                         63, 200, 1000, 1020 };
        unsigned n;

        assert_true(first >= 0);
        for (n = 0; n < 24u; n++) {
            size_t o;

            for (o = 0; o < sizeof offs / sizeof offs[0]; o++) {
                size_t pos = (size_t)first + (size_t)n * bs + offs[o];
                ext4_vol v;
                mem_src src;
                mem_ledger led;
                ext4_jinfo info;

                if (pos >= f->len)
                    continue;
                f->bytes[pos] ^= 0x5Au;
                if (mount_fixture(f, &v, &src, &led, &info) == EXT4_OK) {
                    ext4_dir root;

                    if (ext4_dir_open_root(&v, &root) == EXT4_OK) {
                        ext4_dirent e;

                        while (ext4_dir_next(&root, &e) == 1)
                            ;
                    }
                    led_release(&led);
                } else {
                    assert_int_equal(led.gets, led.puts);
                }
                f->bytes[pos] ^= 0x5Au;
            }
        }
    }
}

/* ------------------------------------------------ superblock refusals */

/* crc32c computed bitwise, independent of falcon-core/src/fs/crc32c.c, so a
 * superblock re-signed here is not signed by the routine under test. */
static uint32_t crc32c_ref(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    unsigned b;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8u; b++)
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

static uint32_t get_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The journal superblock's checksum, over its 1024 bytes with the field
 * zeroed (jbd2_superblock_csum). */
static void jsb_resign(uint8_t *s)
{
    put_be(s + 0xFC, 0);
    put_be(s + 0xFC, crc32c_ref(0xFFFFFFFFu, s, 1024));
}

/* Mounts f with one big-endian field of its journal superblock set to val,
 * re-signed when resign is set; the image is put back afterwards. */
static int mount_jsb_patched(fixture *f, uint32_t bs, uint32_t field,
                             uint32_t val, int resign, ext4_jinfo *info)
{
    long at = find_jblock(f, bs, 4u, ~0u, 0);
    uint8_t save[1024];
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    int rc;

    assert_true(at >= 0);
    memcpy(save, f->bytes + at, sizeof save);
    put_be(f->bytes + at + field, val);
    if (resign)
        jsb_resign(f->bytes + at);
    rc = mount_fixture(f, &v, &src, &led, info);
    if (rc == EXT4_OK)
        led_release(&led);
    else
        assert_int_equal(led.gets, led.puts);
    memcpy(f->bytes + at, save, sizeof save);
    return rc;
}

static void test_jbd2_superblock_refusals(void **state)
{
    long at = find_jblock(&fx_csum, 4096u, 4u, ~0u, 0);
    const uint8_t *s;
    uint32_t maxlen;
    uint32_t incompat;
    ext4_jinfo info;

    (void)state;
    assert_true(at >= 0);
    s = fx_csum.bytes + at;
    maxlen = get_be(s + 0x10);
    incompat = get_be(s + 0x28);

    /* An empty log under a recover bit: nothing to read but the disk, as
     * the kernel mounts it. */
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x1C, 0, 1, &info),
                     EXT4_OK);
    assert_int_equal(info.flags, 0);
    assert_int_equal(info.need, 0);

    /* Format: version 1, a block size not the filesystem's, unknown bits,
     * both checksum versions at once, a checksum type not crc32c. */
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x04, 3, 1, &info),
                     EXT4_ERR_UNSUPPORTED);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x0C, 1024, 1, &info),
                     EXT4_ERR_UNSUPPORTED);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x28,
                                       incompat | 0x80u, 1, &info),
                     EXT4_ERR_UNSUPPORTED);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x2C, 1, 1, &info),
                     EXT4_ERR_UNSUPPORTED);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x28,
                                       incompat | 0x08u, 1, &info),
                     EXT4_ERR_JOURNAL);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x50, 0x01000000u, 1,
                                       &info), EXT4_ERR_UNSUPPORTED);

    /* Damage: the magic, the checksum itself, the geometry. */
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x00, 0, 1, &info),
                     EXT4_ERR_JOURNAL);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x40, 7, 0, &info),
                     EXT4_ERR_JOURNAL);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x10, maxlen + 1u, 1,
                                       &info), EXT4_ERR_JOURNAL);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x10, 0, 1, &info),
                     EXT4_ERR_JOURNAL);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x14, 0, 1, &info),
                     EXT4_ERR_JOURNAL);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x14, maxlen, 1,
                                       &info), EXT4_ERR_JOURNAL);
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x1C, maxlen, 1,
                                       &info), EXT4_ERR_JOURNAL);
    /* A fast-commit area that leaves the log less than the kernel's floor. */
    assert_int_equal(mount_jsb_patched(&fx_csum, 4096u, 0x28,
                                       incompat | 0x20u, 1, &info),
                     EXT4_ERR_JOURNAL);

    /* v1 checksums, on the journal that has no v2 or v3 to outrank them. */
    assert_int_equal(mount_jsb_patched(&fx_nocsum, 1024u, 0x24, 1, 0, &info),
                     EXT4_ERR_UNSUPPORTED);
}

/* A filesystem that says recover but has no journal to recover from. */
static void test_jbd2_no_journal(void **state)
{
    ext4_vol v;
    mem_src src;
    mem_ledger led;
    ext4_jinfo info;
    uint8_t save[1024];
    uint8_t *sb = fx_csum.bytes + 1024;
    uint32_t compat;
    uint32_t c;

    (void)state;
    memcpy(save, sb, sizeof save);
    compat = (uint32_t)sb[0x5C] | ((uint32_t)sb[0x5D] << 8) |
             ((uint32_t)sb[0x5E] << 16) | ((uint32_t)sb[0x5F] << 24);
    compat &= ~0x0004u;
    sb[0x5C] = (uint8_t)compat;
    sb[0x5D] = (uint8_t)(compat >> 8);
    c = crc32c_ref(0xFFFFFFFFu, sb, 0x3FC);
    sb[0x3FC] = (uint8_t)c;
    sb[0x3FD] = (uint8_t)(c >> 8);
    sb[0x3FE] = (uint8_t)(c >> 16);
    sb[0x3FF] = (uint8_t)(c >> 24);
    assert_int_equal(mount_fixture(&fx_csum, &v, &src, &led, &info),
                     EXT4_ERR_CORRUPT);
    assert_int_equal(led.gets, 0);
    memcpy(sb, save, sizeof save);
}

int run_jbd2_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_jbd2_csum_matches_kernel),
        cmocka_unit_test(test_jbd2_nocsum_matches_kernel),
        cmocka_unit_test(test_jbd2_fixtures_meaningful),
        cmocka_unit_test(test_jbd2_fast_commit_skipped),
        cmocka_unit_test(test_jbd2_memory),
        cmocka_unit_test(test_jbd2_io_failure_sweep),
        cmocka_unit_test(test_jbd2_descriptor_damage),
        cmocka_unit_test(test_jbd2_commit_damage),
        cmocka_unit_test(test_jbd2_copy_damage),
        cmocka_unit_test(test_jbd2_corruption_sweep),
        cmocka_unit_test(test_jbd2_superblock_refusals),
        cmocka_unit_test(test_jbd2_no_journal),
    };
    int failed;

    if (load(&fx_csum) != 0 || load(&fx_nocsum) != 0 || load(&fx_fc) != 0) {
        printf("\n[  SKIPPED ] ext4 journal fixtures: build/test-jbd2-*.img "
               "not found.\n"
               "             Build them with: make test-jbd2 "
               "(needs mkfs.ext4 and sudo).\n");
        free(fx_csum.bytes);
        free(fx_nocsum.bytes);
        free(fx_fc.bytes);
        return test_skip_rc();
    }
    failed = cmocka_run_group_tests_name("ext4 journal", tests, NULL, NULL);
    free(fx_csum.bytes);
    free(fx_nocsum.bytes);
    free(fx_fc.bytes);
    return failed;
}
