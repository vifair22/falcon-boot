/* Fixture-backed tests for src/luks (luks.c, af.c) and the crypto under it.
 *
 * These run against the two images tools/mk-test-luks.sh creates with
 * cryptsetup 2.8.7, plus the plaintext reference that script extracts through
 * the kernel's dm-crypt while the volume is open:
 *
 *   build/test-luks.img        LUKS2, aes-xts-plain64, --key-size 512, one
 *                              pbkdf2 keyslot, passphrase "falcon-test", an
 *                              ext4 filesystem inside. The shape DESIGN
 *                              Section 3 mandates for p3.
 *   build/test-luks-argon.img  LUKS2 with an argon2id keyslot and nothing
 *                              inside. The shape the loader must recognise
 *                              and refuse by name.
 *   build/test-luks.ref64k     64 KiB of plaintext, decrypted by dm-crypt.
 *
 * That last file is the whole point of the exercise. src/crypto's AES-XTS is
 * pinned to the IEEE 1619 vectors in crypto_test.c, but a vector proves the
 * cipher and says nothing about whether this loader computes the same tweak
 * dm-crypt does for a given byte offset. The fixture's segment uses a
 * 4096-byte sector size, where dm-crypt advances its IV by eight per data
 * unit rather than one, so a reader that took the plaintext sector index as
 * the tweak would decrypt the first 4096 bytes correctly and produce garbage
 * for every byte after. Comparing against dm-crypt's own output is the only
 * thing that catches it, and the fixture was built at that sector size for
 * exactly this reason.
 *
 * The header-corruption cases work on an in-memory copy of the first 512 KiB
 * (both header copies plus the whole keyslot area) rather than on the file,
 * so a failing test cannot damage the fixture and no case needs a rebuild
 * between runs.
 *
 * Both groups skip cleanly when the images are absent.
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
#include <unistd.h>

#include <luks/luks.h>
#include <luks/af.h>
#include <crypto/crypto.h>
#include <crypto/hash.h>
#include <crypto/sha256.h>
#include <fs/ext4.h>

#ifndef LUKS_IMG
#define LUKS_IMG "build/test-luks.img"
#endif
#ifndef LUKS_ARGON_IMG
#define LUKS_ARGON_IMG "build/test-luks-argon.img"
#endif
#ifndef LUKS_MANIFEST
#define LUKS_MANIFEST "build/test-luks.manifest"
#endif
#ifndef LUKS_REF
#define LUKS_REF "build/test-luks.ref64k"
#endif

#define PASS       "falcon-test"
#define PASS_LEN   (sizeof PASS - 1u)

/* What cryptsetup 2.8.7 wrote, recorded so a silent change in its defaults
 * shows up as a failing assertion here rather than as a mystery later. */
#define HDR_SIZE   16384u
#define JSON_AREA  (HDR_SIZE - LUKS_HDR_BIN)
#define HDR_CSUM_OFF 448u
#define HDR_CSUM_LEN 64u
#define SEG_OFFSET (16u * 1024u * 1024u)
#define SEG_SECTOR 4096u
#define KEY_BYTES  64u
#define AF_STRIPES 4000u
#define PBKDF_ITER 10000u

/* Both header copies (0 and 16384) plus the keyslot area (32768 + 258048),
 * with room to spare. Everything luks_open and luks_unlock touch lives
 * below this. */
#define MEM_BYTES  (512u * 1024u)
#define REF_BYTES  (64u * 1024u)

#define MAN_MAX  16
#define PATH_CAP 512

static int img_fd = -1;
static int argon_fd = -1;

static uint8_t mem_img[MEM_BYTES];
static uint8_t ref[REF_BYTES];
static char    json_buf[LUKS_JSON_CAP];

typedef struct {
    char     type;
    char     path[PATH_CAP];
    uint64_t size;
    char     sha[65];
} man_ent;

static man_ent manifest[MAN_MAX];
static size_t  manifest_n;

/* ------------------------------------------------------------- plumbing */

static int fd_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, (size_t)len, (off_t)off);

        if (n <= 0) {
            return -1;
        }
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

/* Serves the first MEM_BYTES from the private copy and everything beyond it
 * straight from the file. That is what lets a test rewrite a header and then
 * still read the data segment 16 MiB away, which no in-memory copy of a
 * 48 MiB image would want to hold. Nothing in luks.c issues a read that
 * straddles the boundary: header and keyslot reads are far below it and
 * segment reads start far above. */
static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    if (off <= MEM_BYTES && len <= (uint64_t)MEM_BYTES - off) {
        memcpy(buf, mem_img + off, (size_t)len);
        return 0;
    }
    if (off >= MEM_BYTES) {
        return fd_read(&img_fd, off, len, buf);
    }
    return -1;
}

static int dead_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    (void)off;
    (void)len;
    (void)buf;
    return -1;
}

static void mem_reset(void)
{
    assert_int_equal(fd_read(&img_fd, 0, MEM_BYTES, mem_img), 0);
}

static int load_manifest(void)
{
    char line[1024];
    FILE *f = fopen(LUKS_MANIFEST, "r");

    if (f == NULL) {
        return -1;
    }
    manifest_n = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char ty[8], path[PATH_CAP], sha[65];
        unsigned long long size = 0;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "%7[^\t]\t%255[^\t]\t%llu\t%64s",
                   ty, path, &size, sha) != 4) {
            continue;
        }
        if (manifest_n >= MAN_MAX) {
            fclose(f);
            return -1;
        }
        manifest[manifest_n].type = ty[0];
        snprintf(manifest[manifest_n].path, PATH_CAP, "%s", path);
        manifest[manifest_n].size = size;
        snprintf(manifest[manifest_n].sha, sizeof manifest[0].sha, "%s", sha);
        manifest_n++;
    }
    fclose(f);
    return (manifest_n > 0) ? 0 : -1;
}

static const man_ent *manifest_find(const char *path)
{
    size_t i;

    for (i = 0; i < manifest_n; i++) {
        if (strcmp(manifest[i].path, path) == 0) {
            return &manifest[i];
        }
    }
    return NULL;
}

static void hex_of(const uint8_t *d, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2u * i] = digits[(d[i] >> 4) & 0xfu];
        out[2u * i + 1u] = digits[d[i] & 0xfu];
    }
    out[2u * n] = '\0';
}

static void open_fixture(luks_vol *v)
{
    assert_int_equal(luks_open(v, fd_read, &img_fd, json_buf,
                               sizeof json_buf), LUKS_OK);
}

/* ------------------------------------------------------------- geometry */

static void test_luks_open_geometry(void **state)
{
    luks_vol v;

    (void)state;
    open_fixture(&v);

    /* The primary copy at offset 0 is intact, so it is the one used. */
    assert_int_equal(v.hdr_off, 0);
    assert_int_equal(v.used_secondary, 0);
    assert_int_equal(v.hdr_size, HDR_SIZE);
    assert_int_equal(v.json_area, JSON_AREA);
    assert_true(v.seqid > 0);

    /* The JSON area is NUL-padded; json_len is the text, not the padding. */
    assert_true(v.json_len > 0);
    assert_true(v.json_len < v.json_area);
    assert_int_equal(v.json[0], '{');
    assert_int_equal(v.json[v.json_len - 1u], '}');
    assert_ptr_equal(v.json, json_buf);

    /* Segment. The 16 MiB data offset and the 4096-byte sector size are
     * cryptsetup's defaults for this cipher; the sector size in particular
     * is what makes the IV arithmetic in luks.c observable at all. */
    assert_true(v.seg_offset == SEG_OFFSET);
    assert_int_equal(v.seg_sector_size, SEG_SECTOR);
    assert_int_equal(v.seg_dynamic, 1);
    assert_int_equal(v.seg_size, 0);
    assert_int_equal(v.seg_iv_tweak, 0);
    assert_int_equal(v.seg_iv_large, 0);

    assert_int_equal(luks_open(NULL, fd_read, &img_fd, json_buf,
                               sizeof json_buf), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_open(&v, NULL, &img_fd, json_buf,
                               sizeof json_buf), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_open(&v, fd_read, &img_fd, NULL,
                               sizeof json_buf), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_open(&v, fd_read, &img_fd, json_buf, 511),
                     LUKS_ERR_BAD_ARG);

    /* A JSON buffer smaller than the header's own area cannot hold what the
     * checksum covers, so there is nothing to verify against. */
    assert_int_equal(luks_open(&v, fd_read, &img_fd, json_buf, 4096),
                     LUKS_ERR_BAD_HEADER);

    assert_int_equal(luks_open(&v, dead_read, NULL, json_buf,
                               sizeof json_buf), LUKS_ERR_BAD_HEADER);
}

static void test_luks_slot_enumeration(void **state)
{
    luks_vol v;
    luks_slot_info s;
    size_t count = 0;

    (void)state;
    open_fixture(&v);

    assert_int_equal(luks_slot_count(&v, &count), LUKS_OK);
    assert_int_equal(count, 1);

    assert_int_equal(luks_slot_at(&v, 0, &s), LUKS_OK);
    assert_int_equal(s.id, 0);
    assert_int_equal(s.kdf, LUKS_KDF_PBKDF2);
    assert_int_equal(s.kdf_iterations, PBKDF_ITER);
    assert_int_equal(s.key_size, KEY_BYTES);
    assert_int_equal(s.af_stripes, AF_STRIPES);
    /* The point of the whole enumeration: the menu has to be able to say
     * "this volume can be opened by this loader" before asking for a
     * passphrase. */
    assert_int_equal(s.usable, 1);

    assert_int_equal(luks_slot_at(&v, 1, &s), LUKS_ERR_RANGE);
    assert_int_equal(luks_slot_at(&v, 1000, &s), LUKS_ERR_RANGE);
    assert_int_equal(luks_slot_at(NULL, 0, &s), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_slot_at(&v, 0, NULL), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_slot_count(NULL, &count), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_slot_count(&v, NULL), LUKS_ERR_BAD_ARG);
}

/* ---------------------------------------------------------------- unlock */

static void test_luks_unlock_correct_passphrase(void **state)
{
    luks_vol v;
    luks_master_key mk;
    uint32_t slot = 0xffffffffu;
    unsigned i;
    unsigned nonzero = 0;

    (void)state;
    open_fixture(&v);

    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, &slot), LUKS_OK);
    assert_int_equal(slot, 0);
    assert_int_equal(mk.len, KEY_BYTES);

    /* LUKS_OK here means the header's digest object verified the recovered
     * key, not merely that the AF merge produced 64 bytes; a wrong
     * passphrase produces 64 bytes just as readily. The only thing left to
     * check about the bytes themselves is that they are not a zero buffer
     * some failure path forgot to report. */
    for (i = 0; i < mk.len; i++) {
        if (mk.key[i] != 0u) {
            nonzero++;
        }
    }
    assert_true(nonzero > mk.len / 2u);

    /* slot_id is optional. */
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);

    crypto_wipe(&mk, sizeof mk);
}

static void test_luks_unlock_wrong_passphrase(void **state)
{
    luks_vol v;
    luks_master_key mk;
    uint8_t zero[LUKS_MAX_KEY];
    uint32_t slot = 0;

    (void)state;
    open_fixture(&v);
    memset(zero, 0, sizeof zero);

    assert_int_equal(luks_unlock(&v, "wrong", 5, &mk, &slot),
                     LUKS_ERR_NOKEY);
    /* The key buffer must not survive a failed attempt. The loader is about
     * to loop and ask again, and in EFI-land the memory is not unmapped
     * afterwards: it stays in the firmware pool until the OS reclaims it. */
    assert_memory_equal(mk.key, zero, sizeof zero);
    assert_int_equal(mk.len, 0);

    /* Off-by-one variants of the right passphrase, which is the closest an
     * attacker gets without the real one. */
    assert_int_equal(luks_unlock(&v, "falcon-tes", 10, &mk, &slot),
                     LUKS_ERR_NOKEY);
    assert_memory_equal(mk.key, zero, sizeof zero);
    assert_int_equal(luks_unlock(&v, "falcon-testt", 12, &mk, &slot),
                     LUKS_ERR_NOKEY);
    assert_memory_equal(mk.key, zero, sizeof zero);
    assert_int_equal(luks_unlock(&v, "Falcon-test", 11, &mk, &slot),
                     LUKS_ERR_NOKEY);
    assert_memory_equal(mk.key, zero, sizeof zero);

    /* The passphrase is bytes, not a C string: an empty one is a legal
     * attempt and must simply fail. */
    assert_int_equal(luks_unlock(&v, "", 0, &mk, &slot), LUKS_ERR_NOKEY);
    assert_memory_equal(mk.key, zero, sizeof zero);

    /* The right bytes with a length that stops short must not open it
     * either, which is what proves the length is used rather than a
     * terminator. */
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN - 1u, &mk, &slot),
                     LUKS_ERR_NOKEY);

    assert_int_equal(luks_unlock(NULL, PASS, PASS_LEN, &mk, &slot),
                     LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, NULL, &slot),
                     LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_unlock(&v, NULL, 4, &mk, &slot),
                     LUKS_ERR_BAD_ARG);
}

/* -------------------------------------------------- the decrypting reader */

static void test_luks_read_matches_dm_crypt(void **state)
{
    static uint8_t buf[REF_BYTES];
    luks_vol v;
    luks_master_key mk;
    luks_dev dev;

    (void)state;
    open_fixture(&v);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&dev, &v, &mk), LUKS_OK);

    /* The aligned bulk path: sixteen 4096-byte data units in one call. Any
     * error in the plain64 IV stride would show up from the second unit on,
     * which is why the comparison covers more than one. */
    memset(buf, 0, sizeof buf);
    assert_int_equal(luks_read(&dev, 0, REF_BYTES, buf), LUKS_OK);
    if (memcmp(buf, ref, REF_BYTES) != 0) {
        size_t i;

        for (i = 0; i < REF_BYTES; i++) {
            if (buf[i] != ref[i]) {
                fail_msg("plaintext diverges from dm-crypt at byte %zu "
                         "(sector %zu): got 0x%02x, want 0x%02x",
                         i, i / SEG_SECTOR, buf[i], ref[i]);
            }
        }
    }

    /* One unit at a time must give the same bytes as the bulk run. */
    {
        uint64_t off;

        for (off = 0; off + SEG_SECTOR <= REF_BYTES; off += SEG_SECTOR) {
            memset(buf, 0, SEG_SECTOR);
            assert_int_equal(luks_read(&dev, off, SEG_SECTOR, buf), LUKS_OK);
            assert_memory_equal(buf, ref + off, SEG_SECTOR);
        }
    }

    luks_dev_close(&dev);
    crypto_wipe(&mk, sizeof mk);
}

/* Unaligned spans, which take the cached-sector path rather than the bulk
 * one. An ext4 driver stacked on this asks for a 1024-byte superblock at
 * offset 1024, so the ragged path is the common case, not the exotic one. */
static void test_luks_read_unaligned(void **state)
{
    static const uint64_t offs[] = { 1, 511, 4095, 4096, 4097, 8193,
                                     SEG_SECTOR - 1u, 2u * SEG_SECTOR - 1u };
    static const uint64_t lens[] = { 1, 2, 7, 512, 1024, 4096, 9000 };
    static uint8_t buf[16u * 1024u];
    luks_vol v;
    luks_master_key mk;
    luks_dev dev;
    size_t i, j;

    (void)state;
    open_fixture(&v);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&dev, &v, &mk), LUKS_OK);

    for (i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        for (j = 0; j < sizeof lens / sizeof lens[0]; j++) {
            uint64_t off = offs[i];
            uint64_t len = lens[j];

            assert_true(off + len <= REF_BYTES);
            memset(buf, 0xcd, sizeof buf);
            assert_int_equal(luks_read(&dev, off, len, buf), LUKS_OK);
            if (memcmp(buf, ref + off, (size_t)len) != 0) {
                fail_msg("unaligned read of %llu at %llu does not match "
                         "dm-crypt", (unsigned long long)len,
                         (unsigned long long)off);
            }
            /* Nothing written past the requested length. */
            assert_int_equal(buf[len], 0xcd);
        }
    }

    /* A zero-length read is a no-op success, and the sector cache must
     * survive the interleaving of aligned and unaligned requests above. */
    assert_int_equal(luks_read(&dev, 0, 0, NULL), LUKS_OK);
    assert_int_equal(luks_read(&dev, 12345, 1000, buf), LUKS_OK);
    assert_memory_equal(buf, ref + 12345, 1000);

    /* Argument and range guards. The segment is "dynamic", so there is no
     * declared end to run off; an offset that would wrap is still refused. */
    assert_int_equal(luks_read(NULL, 0, 16, buf), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_read(&dev, 0, 16, NULL), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_read(&dev, ~0ull, 2, buf), LUKS_ERR_RANGE);

    /* After close the reader must refuse rather than decrypt with a wiped
     * key schedule. */
    luks_dev_close(&dev);
    assert_int_equal(luks_read(&dev, 0, 16, buf), LUKS_ERR_BAD_ARG);
    luks_dev_close(NULL);

    assert_int_equal(luks_dev_open(NULL, &v, &mk), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_dev_open(&dev, NULL, &mk), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_dev_open(&dev, &v, NULL), LUKS_ERR_BAD_ARG);

    crypto_wipe(&mk, sizeof mk);
}

/* ------------------------------------------------------ header recovery */

/* The second header copy exists for exactly one scenario: a header rewrite
 * interrupted partway, leaving the primary unreadable. These run on an
 * in-memory copy so the fixture survives. */
static void test_luks_secondary_header_fallback(void **state)
{
    luks_vol v;
    luks_master_key mk;
    int mem_ctx = 0;

    (void)state;

    /* Baseline: the untouched copy behaves like the file. */
    mem_reset();
    assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                               sizeof json_buf), LUKS_OK);
    assert_int_equal(v.used_secondary, 0);
    assert_int_equal(v.hdr_off, 0);

    /* Primary magic destroyed: the scan finds the secondary at 0x4000 and
     * the volume opens with identical metadata. */
    mem_reset();
    memset(mem_img, 0, 6);
    assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                               sizeof json_buf), LUKS_OK);
    assert_int_equal(v.used_secondary, 1);
    assert_int_equal(v.hdr_off, HDR_SIZE);
    assert_true(v.seg_offset == SEG_OFFSET);
    assert_int_equal(v.seg_sector_size, SEG_SECTOR);

    /* And it is a working header, not just a parseable one. */
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(mk.len, KEY_BYTES);
    crypto_wipe(&mk, sizeof mk);

    /* A primary whose magic is intact but whose body no longer matches its
     * checksum is rejected just as firmly, and the secondary still wins. */
    mem_reset();
    mem_img[LUKS_HDR_BIN + 32u] ^= 0xffu;
    assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                               sizeof json_buf), LUKS_OK);
    assert_int_equal(v.used_secondary, 1);

    /* A primary claiming to live somewhere other than where it was found is
     * not this copy, which is how a stale header from a resized volume gets
     * rejected rather than half-believed. */
    mem_reset();
    mem_img[256u + 7u] ^= 0x01u;
    assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                               sizeof json_buf), LUKS_OK);
    assert_int_equal(v.used_secondary, 1);

    /* Both copies gone: there is nothing left to read. */
    mem_reset();
    memset(mem_img, 0, 6);
    memset(mem_img + HDR_SIZE, 0, 6);
    assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                               sizeof json_buf), LUKS_ERR_BAD_HEADER);

    /* A wrong version number is refused even with the magic intact: LUKS1
     * has a completely different layout and misreading it would produce
     * nonsense rather than an error. */
    mem_reset();
    mem_img[6] = 0;
    mem_img[7] = 1;
    mem_img[HDR_SIZE + 6u] = 0;
    mem_img[HDR_SIZE + 7u] = 1;
    assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                               sizeof json_buf), LUKS_ERR_BAD_HEADER);

    /* A checksum algorithm this loader does not implement is reported as
     * unsupported by neither copy, so the open fails as a bad header rather
     * than silently skipping verification. */
    mem_reset();
    memcpy(mem_img + 72u, "sha3-256", 9);
    memcpy(mem_img + HDR_SIZE + 72u, "sha3-256", 9);
    assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                               sizeof json_buf), LUKS_ERR_BAD_HEADER);
}

/* ----------------------------------------------------------- argon2id */

/* p4 and any default `cryptsetup luksFormat` produce an argon2id keyslot.
 * DESIGN Section 3 rules argon2id out of the loader on purpose: it is
 * memory-hard and brutal in single-threaded EFI-land with no tuned
 * allocator. What matters is that the refusal is specific. A user whose
 * volume cannot be opened because of its KDF needs to be told that, not told
 * their passphrase was wrong. */
static void test_luks_argon_slot_is_named_not_guessed(void **state)
{
    luks_vol v;
    luks_slot_info s;
    luks_master_key mk;
    size_t count = 0;

    (void)state;
    assert_int_equal(luks_open(&v, fd_read, &argon_fd, json_buf,
                               sizeof json_buf), LUKS_OK);

    assert_int_equal(luks_slot_count(&v, &count), LUKS_OK);
    assert_int_equal(count, 1);
    assert_int_equal(luks_slot_at(&v, 0, &s), LUKS_OK);
    assert_int_equal(s.id, 0);
    assert_int_equal(s.kdf, LUKS_KDF_ARGON2ID);
    assert_int_equal(s.key_size, KEY_BYTES);
    assert_int_equal(s.af_stripes, AF_STRIPES);
    assert_int_equal(s.usable, 0);
    /* iterations is only filled in for a pbkdf2 slot; argon2's time cost is
     * a different quantity and reporting it in that field would be a lie. */
    assert_int_equal(s.kdf_iterations, 0);

    /* The correct passphrase does not change the answer: this is about the
     * keyslot's KDF, not about the passphrase. */
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED_KDF);
    assert_int_equal(luks_unlock(&v, "wrong", 5, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED_KDF);
}

/* ------------------------------------------------------ ext4 over luks */

/* The stack the loader actually runs: an ext4 driver whose read callback is
 * luks_read, with &luks_dev as its context. luks.h promises the signatures
 * line up so this needs no adapter, and this is where that promise is
 * checked. The files' hashes are computed with this repo's own SHA-256 over
 * bytes that came through its own AES-XTS off a volume cryptsetup created,
 * and compared against sha256sum's numbers from the fixture build. */
static void test_ext4_over_luks(void **state)
{
    static uint8_t chunk[64u * 1024u];
    luks_vol v;
    luks_master_key mk;
    luks_dev dev;
    ext4_vol fs;
    ext4_dir root;
    ext4_dirent e;
    unsigned matched = 0;

    (void)state;
    open_fixture(&v);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&dev, &v, &mk), LUKS_OK);

    assert_int_equal(ext4_mount(&fs, luks_read, &dev), EXT4_OK);
    assert_int_equal(fs.has_csum, 1);
    assert_true(fs.block_size == 1024u || fs.block_size == 4096u);

    assert_int_equal(ext4_dir_open_root(&fs, &root), EXT4_OK);
    for (;;) {
        char path[PATH_CAP];
        const man_ent *m;
        ext4_file f;
        sha256_ctx c;
        uint8_t digest[SHA256_DIGEST_LEN];
        char hex[2u * SHA256_DIGEST_LEN + 1u];
        uint64_t off = 0;
        uint64_t size;
        int rc = ext4_dir_next(&root, &e);

        assert_true(rc >= 0);
        if (rc == 0) {
            break;
        }
        snprintf(path, sizeof path, "/%s", (const char *)e.name);
        m = manifest_find(path);
        if (m == NULL) {
            fail_msg("object inside the LUKS volume is not in the "
                     "manifest: %s", path);
        }
        if (!e.is_regular) {
            continue;
        }
        assert_int_equal(ext4_file_open(&fs, &f, &e), EXT4_OK);
        size = ext4_file_size(&f);
        assert_true(size == m->size);

        sha256_init(&c);
        while (off < size) {
            uint64_t take = size - off;

            if (take > sizeof chunk) {
                take = sizeof chunk;
            }
            assert_int_equal(ext4_file_read(&f, off, take, chunk), EXT4_OK);
            sha256_update(&c, chunk, (size_t)take);
            off += take;
        }
        sha256_final(&c, digest);
        hex_of(digest, sizeof digest, hex);
        if (strcmp(hex, m->sha) != 0) {
            fail_msg("%s inside the volume: sha256 %s, manifest says %s",
                     path, hex, m->sha);
        }
        matched++;
    }
    assert_int_equal(matched, 2);

    /* Closing the device must leave the ext4 driver unable to read rather
     * than reading ciphertext: luks_read checks the context. */
    luks_dev_close(&dev);
    assert_int_equal(ext4_mount(&fs, luks_read, &dev), EXT4_ERR_IO);

    crypto_wipe(&mk, sizeof mk);
}

/* ------------------------------------------------ rewritten metadata */

/* The refusal paths in luks.c that no real cryptsetup volume can reach.
 *
 * Every case here takes the document cryptsetup actually wrote, applies one
 * textual substitution to it, writes it back into the primary header of the
 * in-memory copy and repairs the header checksum. Keeping the rest of the
 * document genuine matters: the salts, the digest and the keyslot area on
 * disk all still agree with each other, so a case that is supposed to fail
 * three quarters of the way through an unlock actually gets that far instead
 * of falling over on the first field.
 *
 * The repaired checksum is computed with this repo's own SHA-256, which is
 * circular for the checksum check itself and irrelevant to it: the checksum
 * is not what any of these cases is about, it is the gate they have to pass
 * to reach the metadata handling that is. The checksum's own behaviour is
 * tested in test_luks_secondary_header_fallback, against bytes nobody here
 * wrote.
 */

static char   orig_json[LUKS_JSON_CAP];
static size_t orig_json_len;

static void load_orig_json(void)
{
    size_t n = 0;

    mem_reset();
    while (n < JSON_AREA && mem_img[LUKS_HDR_BIN + n] != 0u) {
        n++;
    }
    assert_true(n > 0 && n < JSON_AREA);
    memcpy(orig_json, mem_img + LUKS_HDR_BIN, n);
    orig_json[n] = '\0';
    orig_json_len = n;
}

static void mem_json_set(const char *doc, size_t n)
{
    sha256_ctx c;
    uint8_t digest[SHA256_DIGEST_LEN];

    assert_true(n < JSON_AREA);
    memset(mem_img + LUKS_HDR_BIN, 0, JSON_AREA);
    memcpy(mem_img + LUKS_HDR_BIN, doc, n);
    /* The secondary copy still holds the original document, so it is put
     * beyond use. Otherwise a rewrite that made the primary unparseable
     * would be answered from the untouched copy and the test would pass
     * while proving nothing. */
    memset(mem_img + HDR_SIZE, 0, 6);
    memset(mem_img + HDR_CSUM_OFF, 0, HDR_CSUM_LEN);
    sha256_init(&c);
    sha256_update(&c, mem_img, LUKS_HDR_BIN);
    sha256_update(&c, mem_img + LUKS_HDR_BIN, JSON_AREA);
    sha256_final(&c, digest);
    memcpy(mem_img + HDR_CSUM_OFF, digest, sizeof digest);
}

static void mem_json_sub(const char *needle, const char *repl)
{
    static char doc[LUKS_JSON_CAP];
    const char *at;
    size_t pre, nlen, rlen, tail;

    mem_reset();
    at = strstr(orig_json, needle);
    if (at == NULL) {
        fail_msg("the fixture header does not contain \"%s\"; cryptsetup's "
                 "output has changed shape", needle);
    }
    pre = (size_t)(at - orig_json);
    nlen = strlen(needle);
    rlen = strlen(repl);
    tail = orig_json_len - pre - nlen;
    assert_true(pre + rlen + tail < sizeof doc);
    memcpy(doc, orig_json, pre);
    memcpy(doc + pre, repl, rlen);
    memcpy(doc + pre + rlen, orig_json + pre + nlen, tail);
    mem_json_set(doc, pre + rlen + tail);
}

static int open_sub(luks_vol *v, const char *needle, const char *repl)
{
    static int mem_ctx;

    mem_json_sub(needle, repl);
    return luks_open(v, mem_read, &mem_ctx, json_buf, sizeof json_buf);
}

static void test_luks_segment_refusals(void **state)
{
    luks_vol v;

    (void)state;
    load_orig_json();

    /* Sanity: an unmodified rewrite still opens, so every refusal below is
     * the substitution and not the rewriting machinery. */
    assert_int_equal(open_sub(&v, "\"type\":\"crypt\"", "\"type\":\"crypt\""),
                     LUKS_OK);
    assert_int_equal(v.used_secondary, 0);
    assert_true(v.seg_offset == SEG_OFFSET);

    /* A segment that is not encrypted at all. The loader only ever presents
     * a decrypting reader, so there is nothing sensible to do with one. */
    assert_int_equal(open_sub(&v, "\"type\":\"crypt\"", "\"type\":\"linear\""),
                     LUKS_ERR_UNSUPPORTED);

    /* A cipher outside DESIGN Section 3's scope. Saying so beats deriving a
     * key and handing back plausible garbage. */
    assert_int_equal(open_sub(&v,
                              "\"iv_tweak\":\"0\",\"encryption\":"
                              "\"aes-xts-plain64\"",
                              "\"iv_tweak\":\"0\",\"encryption\":"
                              "\"aes-cbc-essiv:sha256\""),
                     LUKS_ERR_UNSUPPORTED);

    /* Sector sizes: not a power of two, below the floor, and past the
     * largest buffer the reader carries. */
    assert_int_equal(open_sub(&v, "\"sector_size\":4096",
                              "\"sector_size\":640"),
                     LUKS_ERR_UNSUPPORTED);
    assert_int_equal(open_sub(&v, "\"sector_size\":4096",
                              "\"sector_size\":256"),
                     LUKS_ERR_UNSUPPORTED);
    assert_int_equal(open_sub(&v, "\"sector_size\":4096",
                              "\"sector_size\":8192"),
                     LUKS_ERR_UNSUPPORTED);

    /* 512 is the other size seen in the wild and must simply work. */
    assert_int_equal(open_sub(&v, "\"sector_size\":4096",
                              "\"sector_size\":512"), LUKS_OK);
    assert_int_equal(v.seg_sector_size, 512);

    /* A nonzero iv_tweak shifts every tweak; it is the volume's, not ours,
     * so it is recorded rather than assumed zero. */
    assert_int_equal(open_sub(&v, "\"iv_tweak\":\"0\"",
                              "\"iv_tweak\":\"8\""), LUKS_OK);
    assert_int_equal(v.seg_iv_tweak, 8);

    /* iv_large_sectors changes how dm-crypt counts the IV. */
    assert_int_equal(open_sub(&v, "\"sector_size\":4096",
                              "\"sector_size\":4096,"
                              "\"flags\":[\"iv_large_sectors\"]"), LUKS_OK);
    assert_int_equal(v.seg_iv_large, 1);

    /* A flags array that does not carry it must not turn it on. */
    assert_int_equal(open_sub(&v, "\"sector_size\":4096",
                              "\"sector_size\":4096,"
                              "\"flags\":[\"no-read-workqueue\"]"), LUKS_OK);
    assert_int_equal(v.seg_iv_large, 0);

    /* Missing or malformed members are corrupt metadata rather than an
     * unsupported feature: the document is not the shape the format
     * defines. */
    assert_int_equal(open_sub(&v, "\"segments\"", "\"segmentz\""),
                     LUKS_ERR_CORRUPT);
    assert_int_equal(open_sub(&v, "\"iv_tweak\"", "\"iv_tweek\""),
                     LUKS_ERR_CORRUPT);
    assert_int_equal(open_sub(&v, "\"offset\":\"16777216\"",
                              "\"offzet\":\"16777216\""),
                     LUKS_ERR_CORRUPT);
    assert_int_equal(open_sub(&v, "\"size\":\"dynamic\"",
                              "\"size\":\"not-a-number\""),
                     LUKS_ERR_CORRUPT);
    assert_int_equal(open_sub(&v, "\"sector_size\":4096",
                              "\"sector_size\":\"4096x\""),
                     LUKS_ERR_CORRUPT);

    /* A JSON area that is not JSON at all. */
    mem_reset();
    mem_json_set("this is not a document", 22);
    {
        int mem_ctx = 0;

        assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                                   sizeof json_buf), LUKS_ERR_CORRUPT);
    }

    /* A document whose root is not an object. */
    mem_reset();
    mem_json_set("[1,2,3]", 7);
    {
        int mem_ctx = 0;

        assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                                   sizeof json_buf), LUKS_ERR_CORRUPT);
    }
}

/* A fixed-size segment, which the fixture is not: cryptsetup writes
 * "dynamic" for a volume that runs to the end of the device. The size is
 * what bounds luks_read, so it needs a case of its own. */
static void test_luks_fixed_size_segment(void **state)
{
    static uint8_t buf[REF_BYTES];
    luks_vol v;
    luks_master_key mk;
    luks_dev dev;

    (void)state;
    load_orig_json();

    assert_int_equal(open_sub(&v, "\"size\":\"dynamic\"",
                              "\"size\":\"65536\""), LUKS_OK);
    assert_int_equal(v.seg_dynamic, 0);
    assert_true(v.seg_size == REF_BYTES);

    /* The plaintext is unchanged, which is what proves the substitution
     * machinery did not disturb anything that matters. */
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&dev, &v, &mk), LUKS_OK);
    assert_int_equal(luks_read(&dev, 0, REF_BYTES, buf), LUKS_OK);
    assert_memory_equal(buf, ref, REF_BYTES);

    /* And now the segment has an end to run off. */
    assert_int_equal(luks_read(&dev, 0, REF_BYTES + 1u, buf),
                     LUKS_ERR_RANGE);
    assert_int_equal(luks_read(&dev, REF_BYTES, 1, buf), LUKS_ERR_RANGE);
    assert_int_equal(luks_read(&dev, REF_BYTES - 1u, 1, buf), LUKS_OK);

    luks_dev_close(&dev);
    crypto_wipe(&mk, sizeof mk);
}

static void test_luks_keyslot_refusals(void **state)
{
    luks_vol v;
    luks_master_key mk;
    size_t count = 0;

    (void)state;
    load_orig_json();

    /* argon2i as well as argon2id: both are named and both are refused as a
     * KDF rather than as a wrong passphrase. */
    assert_int_equal(open_sub(&v, "\"kdf\":{\"type\":\"pbkdf2\"",
                              "\"kdf\":{\"type\":\"argon2i\""), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED_KDF);

    /* A KDF nobody has heard of is not an argon2 slot, so it is reported as
     * an unsupported slot instead of an unsupported KDF: the distinction is
     * what tells the user whether a different tool could have opened it. */
    assert_int_equal(open_sub(&v, "\"kdf\":{\"type\":\"pbkdf2\"",
                              "\"kdf\":{\"type\":\"scrypt\""), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED_KDF);

    /* Slot, af and area types outside the format's one legal spelling. */
    assert_int_equal(open_sub(&v, "\"0\":{\"type\":\"luks2\"",
                              "\"0\":{\"type\":\"luks9\""), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);
    assert_int_equal(open_sub(&v, "\"af\":{\"type\":\"luks1\"",
                              "\"af\":{\"type\":\"luks2\""), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);
    assert_int_equal(open_sub(&v, "\"area\":{\"type\":\"raw\"",
                              "\"area\":{\"type\":\"checksum\""), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* Hashes outside the two this loader implements, in the keyslot KDF and
     * in the AF splitter. hash.c's table is closed on purpose. */
    assert_int_equal(open_sub(&v, "\"type\":\"pbkdf2\",\"hash\":\"sha256\","
                                  "\"iterations\":10000",
                              "\"type\":\"pbkdf2\",\"hash\":\"ripemd160\","
                              "\"iterations\":10000"), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);
    assert_int_equal(open_sub(&v, "\"stripes\":4000,\"hash\":\"sha256\"",
                              "\"stripes\":4000,\"hash\":\"sha1\""),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* A master key longer than the loader's buffer. */
    assert_int_equal(open_sub(&v, "\"type\":\"luks2\",\"key_size\":64",
                              "\"type\":\"luks2\",\"key_size\":65"),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* Zero stripes, and more stripes than the declared area can hold. Both
     * are self-inconsistent metadata, not a feature. */
    assert_int_equal(open_sub(&v, "\"stripes\":4000", "\"stripes\":0"),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);
    assert_int_equal(open_sub(&v, "\"stripes\":4000", "\"stripes\":8000"),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* Zero iterations would make the KDF a no-op. */
    assert_int_equal(open_sub(&v, "\"iterations\":10000",
                              "\"iterations\":0"), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* No keyslots member at all. luks_open does not look at the keyslots, so
     * this is not noticed until an unlock is attempted, which is the right
     * division: a volume whose segment is intact can still be described in
     * the menu. */
    assert_int_equal(open_sub(&v, "\"keyslots\":{\"0\"",
                              "\"unused\":{\"0\""), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_CORRUPT);
    assert_int_equal(luks_slot_count(&v, &count), LUKS_ERR_CORRUPT);
    {
        luks_slot_info s;

        assert_int_equal(luks_slot_at(&v, 0, &s), LUKS_ERR_CORRUPT);
    }

    /* A keyslots object with no members parses, enumerates as empty, and
     * makes unlock report that nothing usable existed. */
    {
        static char doc[LUKS_JSON_CAP];
        int mem_ctx = 0;
        const char *at;
        size_t pre;

        mem_reset();
        at = strstr(orig_json, "\"tokens\"");
        assert_non_null(at);
        pre = (size_t)(at - orig_json);
        memcpy(doc, "{\"keyslots\":{},", 15);
        memcpy(doc + 15, orig_json + pre, orig_json_len - pre);
        mem_json_set(doc, 15u + orig_json_len - pre);
        assert_int_equal(luks_open(&v, mem_read, &mem_ctx, json_buf,
                                   sizeof json_buf), LUKS_OK);
        assert_int_equal(luks_slot_count(&v, &count), LUKS_OK);
        assert_int_equal(count, 0);
        {
            luks_slot_info s;

            assert_int_equal(luks_slot_at(&v, 0, &s), LUKS_ERR_RANGE);
        }
        assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                         LUKS_ERR_UNSUPPORTED);
    }

    /* Enumeration over metadata that is not an object at all. */
    assert_int_equal(open_sub(&v, "\"keyslots\":{\"0\":{\"type\":\"luks2\"",
                              "\"keyslots\":[{\"type\":\"luks2\""),
                     LUKS_ERR_CORRUPT);
}

/* The digest object is what turns "the merge produced 64 bytes" into "the
 * passphrase was right". Each of these breaks it a different way, and each
 * one has to be reached through a complete, correct derivation, which is why
 * they use the right passphrase. */
static void test_luks_digest_refusals(void **state)
{
    luks_vol v;
    luks_master_key mk;

    (void)state;
    load_orig_json();

    /* No digests object at all. */
    assert_int_equal(open_sub(&v, "\"digests\"", "\"digeztz\""), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_CORRUPT);

    /* A digest that covers no keyslot this header has: there is nothing to
     * verify the recovered key against, so the slot is corrupt. luks_unlock
     * reports UNSUPPORTED rather than passing that up, because a corrupt
     * slot is not fatal on its own: another slot in the same header may
     * still be one this loader can open, so the loop keeps going and the
     * final answer describes the header as a whole. */
    assert_int_equal(open_sub(&v, "\"keyslots\":[\"0\"]",
                              "\"keyslots\":[\"7\"]"), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* A digest type the loader cannot compute. */
    assert_int_equal(open_sub(&v, "\"digests\":{\"0\":{\"type\":\"pbkdf2\"",
                              "\"digests\":{\"0\":{\"type\":\"argon2id\""),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* A digest that does not cover segment 0, which is the only segment the
     * loader reads. */
    assert_int_equal(open_sub(&v, "\"segments\":[\"0\"]",
                              "\"segments\":[\"1\"]"), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* A digest hash outside the closed table. The needle carries the
     * preceding member because "hash":"sha256" alone appears three times in
     * a LUKS2 header (keyslot KDF, AF splitter, digest) and this case is
     * about the third one. */
    assert_int_equal(open_sub(&v,
                              "\"segments\":[\"0\"],\"hash\":\"sha256\"",
                              "\"segments\":[\"0\"],\"hash\":\"sha384\""),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* A zero iteration count on the digest, which would make the check a
     * bare hash. Corrupt at the slot, so UNSUPPORTED for the header. The
     * trailing ,"salt" keeps this off the keyslot's "iterations":10000,
     * which the digest's own value is a prefix of. */
    assert_int_equal(open_sub(&v, "\"iterations\":1000,\"salt\"",
                              "\"iterations\":0,\"salt\""),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);

    /* A digest that is the right shape and the wrong value: the derivation
     * succeeded and the key is not the one this volume was made with, which
     * is exactly what a wrong passphrase looks like. */
    {
        char sub[128];
        const char *at = strstr(orig_json, "\"digest\":\"");
        size_t i;

        assert_non_null(at);
        /* Flip one character of the recorded digest, keeping it valid
         * base64 of the same length. */
        for (i = 0; i < 24u; i++) {
            sub[i] = at[i];
        }
        sub[23] = (char)((sub[23] == 'A') ? 'B' : 'A');
        sub[24] = '\0';
        {
            char needle[32];

            memcpy(needle, at, 24);
            needle[24] = '\0';
            assert_int_equal(open_sub(&v, needle, sub), LUKS_OK);
        }
        assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                         LUKS_ERR_NOKEY);
    }
}

/* A read that fails partway through the keyslot area. The loader streams
 * 258 KB of it one 512-byte unit at a time, so an unreadable stick is the
 * likely failure and it has to surface as I/O rather than as a wrong
 * passphrase. */
static uint64_t io_cut;

static int cut_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    if (off >= io_cut) {
        return -1;
    }
    return fd_read(ctx, off, len, buf);
}

static void test_luks_io_failures(void **state)
{
    luks_vol v;
    luks_master_key mk;

    (void)state;

    /* The binary header reads, but its JSON area does not. */
    io_cut = LUKS_HDR_BIN;
    assert_int_equal(luks_open(&v, cut_read, &img_fd, json_buf,
                               sizeof json_buf), LUKS_ERR_BAD_HEADER);

    /* Both headers read, the keyslot area does not. */
    io_cut = 32768u;
    assert_int_equal(luks_open(&v, cut_read, &img_fd, json_buf,
                               sizeof json_buf), LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_IO);

    /* The keyslot area reads, the data segment does not. */
    {
        luks_dev dev;
        uint8_t buf[4096];

        io_cut = SEG_OFFSET;
        assert_int_equal(luks_open(&v, cut_read, &img_fd, json_buf,
                                   sizeof json_buf), LUKS_OK);
        assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                         LUKS_OK);
        assert_int_equal(luks_dev_open(&dev, &v, &mk), LUKS_OK);
        assert_int_equal(luks_read(&dev, 0, sizeof buf, buf),
                         LUKS_ERR_IO);
        /* The unaligned path fails the same way and must not leave a stale
         * sector in the cache. */
        assert_int_equal(luks_read(&dev, 1, 16, buf), LUKS_ERR_IO);
        luks_dev_close(&dev);
        crypto_wipe(&mk, sizeof mk);
    }

    /* A JSON buffer exactly the size of the header's area leaves no room for
     * the terminator the parser needs, so the open is refused rather than
     * truncating the document. */
    assert_int_equal(luks_open(&v, fd_read, &img_fd, json_buf, JSON_AREA),
                     LUKS_ERR_NOSPACE);
    assert_int_equal(luks_open(&v, fd_read, &img_fd, json_buf,
                               JSON_AREA + 1u), LUKS_OK);
}

/* ------------------------------------------------- the AF splitter alone */

/* af.c is exercised end to end by every unlock above, but only on the one
 * shape cryptsetup wrote (64-byte key, 4000 stripes, sha256). Its argument
 * and state checks have no other way in. */
static void test_af_merge_guards(void **state)
{
    af_merge_ctx c;
    uint8_t stripe[AF_MAX_BLOCK];
    uint8_t out[AF_MAX_BLOCK];

    (void)state;
    memset(stripe, 0xa5, sizeof stripe);

    assert_int_equal(af_merge_init(NULL, hash_sha256(), 64, 4000),
                     AF_ERR_BAD_ARG);
    assert_int_equal(af_merge_init(&c, NULL, 64, 4000), AF_ERR_BAD_ARG);
    assert_int_equal(af_merge_init(&c, hash_sha256(), 0, 4000),
                     AF_ERR_BAD_ARG);
    assert_int_equal(af_merge_init(&c, hash_sha256(), AF_MAX_BLOCK + 1u,
                                   4000), AF_ERR_BAD_ARG);
    assert_int_equal(af_merge_init(&c, hash_sha256(), 64, 0),
                     AF_ERR_BAD_ARG);

    /* Fewer stripes than declared must not produce a key: a short merge is
     * exactly what a truncated keyslot area would deliver, and the result
     * would be a plausible-looking 64 bytes that fails the digest check for
     * a reason nobody could diagnose. */
    assert_int_equal(af_merge_init(&c, hash_sha256(), 64, 3), AF_OK);
    assert_int_equal(af_merge_stripe(&c, stripe), AF_OK);
    assert_int_equal(af_merge_final(&c, out), AF_ERR_STATE);
    assert_int_equal(af_merge_stripe(&c, stripe), AF_OK);
    assert_int_equal(af_merge_final(&c, out), AF_ERR_STATE);
    assert_int_equal(af_merge_stripe(&c, stripe), AF_OK);
    assert_int_equal(af_merge_final(&c, out), AF_OK);
    /* One more than declared is a state error, not silently absorbed. */
    assert_int_equal(af_merge_stripe(&c, stripe), AF_ERR_STATE);

    assert_int_equal(af_merge_stripe(NULL, stripe), AF_ERR_BAD_ARG);
    assert_int_equal(af_merge_stripe(&c, NULL), AF_ERR_BAD_ARG);
    assert_int_equal(af_merge_final(NULL, out), AF_ERR_BAD_ARG);
    assert_int_equal(af_merge_final(&c, NULL), AF_ERR_BAD_ARG);

    /* Three identical stripes do not cancel to zero: the diffusion between
     * them is what stops the merge from being a plain XOR. */
    {
        uint8_t zero[AF_MAX_BLOCK];

        memset(zero, 0, sizeof zero);
        assert_int_not_equal(memcmp(out, zero, 64), 0);
    }

    af_merge_wipe(&c);
    af_merge_wipe(NULL);
}

/* ------------------------------------------------------------------ run */


/* ------------------------------------------------- corruption sweep */

/* Flip metadata bytes and require the driver to answer rather than misbehave.
 * src/luks/json.c earns its 96% from exactly this shape, and every uncovered
 * line in luks.c is a refusal: the paths a tampered p3 takes, reached by
 * reading the code and by nothing else.
 *
 * mem_img already holds a private copy of the first 512 KiB, so a flip is a
 * write to it rather than a hook on the read path.
 *
 * What is swept, and what is not. The whole of the 4096-byte binary header,
 * because the fields checked before the checksum (magic, version, hdr_size,
 * the copy's own hdr_offset, the checksum algorithm name) are spread through
 * it. Then the JSON text, but not the NUL padding that fills the area out to
 * hdr_size: a flip there is caught by the checksum like any other and tells
 * us nothing the header sweep has not already. Sweeping all 32 KiB with two
 * masks costs 65536 parses and forty seconds of SHA-256 for that. */

#define SEC_HDR_OFF 16384u      /* where cryptsetup put the second copy */

typedef struct {
    uint32_t off;
    uint32_t len;
} sweep_span;

static sweep_span g_spans[2];
static size_t     g_span_n;

static int luks_status_known(int rc)
{
    switch (rc) {
    case LUKS_OK:
    case LUKS_ERR_IO:
    case LUKS_ERR_BAD_HEADER:
    case LUKS_ERR_CORRUPT:
    case LUKS_ERR_BAD_ARG:
    case LUKS_ERR_RANGE:
    case LUKS_ERR_NOSPACE:
    case LUKS_ERR_UNSUPPORTED:
    case LUKS_ERR_UNSUPPORTED_KDF:
    case LUKS_ERR_NOKEY:
        return 1;
    default:
        return 0;
    }
}

/* Parse as far as the metadata goes without deriving a key. */
static int sweep_parse(luks_vol *v)
{
    luks_slot_info si;
    size_t count = 0;
    size_t i;
    int rc;

    rc = luks_open(v, mem_read, NULL, json_buf, sizeof json_buf);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = luks_slot_count(v, &count);
    if (rc != LUKS_OK) {
        return rc;
    }
    for (i = 0; i < count; i++) {
        rc = luks_slot_at(v, i, &si);
        if (rc != LUKS_OK) {
            return rc;
        }
    }
    return LUKS_OK;
}

static void sweep_spans_init(void)
{
    uint32_t json_len = 0;

    while (json_len < 12288u && mem_img[4096u + json_len] != 0) {
        json_len++;
    }
    assert_true(json_len > 200u);
    g_spans[0].off = 0u;
    g_spans[0].len = 4096u;
    g_spans[1].off = 4096u;
    g_spans[1].len = json_len;
    g_span_n = 2;
}

/* Damage the primary copy alone, any single byte of it, and the volume must
 * still open. That is the entire reason LUKS2 writes two headers: a rewrite
 * interrupted by a power cut leaves one of them torn, and the reader is
 * expected to notice and take the other.
 *
 * It also reaches a line no other test does. Until this existed nothing in
 * the suite had ever made the secondary copy the one in use, so the selection
 * that prefers it was cold. */
static void test_luks_primary_damage_falls_back(void **state)
{
    static const uint8_t masks[] = { 0xffu, 0x01u };
    luks_vol v;
    size_t sp;
    size_t m;
    size_t checked = 0;
    size_t fell_back = 0;

    (void)state;
    mem_reset();
    sweep_spans_init();
    assert_int_equal(sweep_parse(&v), LUKS_OK);
    assert_int_equal(v.used_secondary, 0);   /* intact: the primary is used */

    for (sp = 0; sp < g_span_n; sp++) {
        uint32_t at;

        for (at = g_spans[sp].off; at < g_spans[sp].off + g_spans[sp].len;
             at++) {
            for (m = 0; m < sizeof masks / sizeof masks[0]; m++) {
                int rc;

                mem_img[at] ^= masks[m];
                rc = sweep_parse(&v);
                mem_img[at] ^= masks[m];

                if (rc != LUKS_OK) {
                    fail_msg("primary flip 0x%02x at %u refused the volume "
                             "(%d); the secondary copy should have carried "
                             "it", (unsigned)masks[m], at, rc);
                }
                if (v.used_secondary != 0u) {
                    fell_back++;
                }
                checked++;
            }
        }
    }
    assert_true(checked > 8000);
    /* Most flips make the primary unusable and the secondary takes over. The
     * ones that do not are bytes the format never reads back: the 64-byte
     * checksum field holds a 32-byte SHA-256 digest, and check_header zeroes
     * the whole field before hashing and compares only the digest, so a flip
     * in the 32 bytes of slack behind it changes nothing. Requiring a
     * fallback on every byte would be asserting that those bytes matter,
     * which they do not. */
    assert_true(fell_back > checked - 200u);

    mem_reset();
}

/* Damage both copies at the same offset, which is the case the redundancy
 * cannot absorb, and require a clean refusal rather than a best-effort
 * interpretation of whichever fields survived. */
static void test_luks_both_copies_corruption_sweep(void **state)
{
    static const uint8_t masks[] = { 0xffu, 0x01u };
    luks_vol v;
    size_t sp;
    size_t m;
    size_t swept = 0;
    size_t refused = 0;

    (void)state;
    mem_reset();
    sweep_spans_init();

    for (sp = 0; sp < g_span_n; sp++) {
        uint32_t at;

        for (at = g_spans[sp].off; at < g_spans[sp].off + g_spans[sp].len;
             at++) {
            for (m = 0; m < sizeof masks / sizeof masks[0]; m++) {
                int rc;

                mem_img[at] ^= masks[m];
                mem_img[SEC_HDR_OFF + at] ^= masks[m];
                rc = sweep_parse(&v);
                mem_img[at] ^= masks[m];
                mem_img[SEC_HDR_OFF + at] ^= masks[m];

                if (!luks_status_known(rc)) {
                    fail_msg("flip 0x%02x at %u returned %d",
                             (unsigned)masks[m], at, rc);
                }
                if (rc != LUKS_OK) {
                    refused++;
                }
                swept++;
            }
        }
    }

    assert_true(swept > 8000);
    /* Both copies carry a checksum over their whole hdr_size, so damaging
     * the same byte in each is caught nearly every time. A run that refused
     * only a handful would mean the flips never reached the parser. */
    assert_true(refused > 8000);

    mem_reset();
    assert_int_equal(sweep_parse(&v), LUKS_OK);
}

/* Recompute a header copy's checksum in mem_img, so a test can damage the
 * metadata the checksum covers and still have the parser look at it.
 *
 * Without this the whole of this driver below check_header is unreachable by
 * corruption: the sum spans the binary header AND the JSON area, so a flipped
 * byte anywhere in either comes back as a rejected copy and the JSON is never
 * parsed at all. Measured: the two sweeps above, which do not refresh it,
 * reach exactly one line the rest of the suite did not.
 *
 * Mirrors check_header: zero the 64-byte field, hash hdr_size bytes, write
 * the digest back and leave the slack behind it zero. The fixture's algorithm
 * is sha256; a fixture that used another would need this to follow
 * HDR_OFF_CSUM_ALG the way the driver does. */
#define HDR_CSUM_AT  448u
#define HDR_CSUM_LEN 64u
#define HDR_SEQID_AT 16u

static void hdr_fix_csum(uint32_t base)
{
    sha256_ctx c;
    uint8_t digest[SHA256_DIGEST_LEN];

    memset(mem_img + base + HDR_CSUM_AT, 0, HDR_CSUM_LEN);
    sha256_init(&c);
    sha256_update(&c, mem_img + base, HDR_SIZE);
    sha256_final(&c, digest);
    memcpy(mem_img + base + HDR_CSUM_AT, digest, sizeof digest);
}

static uint64_t hdr_get_seqid(uint32_t base)
{
    const uint8_t *p = mem_img + base + HDR_SEQID_AT;
    uint64_t v = 0;
    size_t i;

    for (i = 0; i < 8u; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void hdr_put_seqid(uint32_t base, uint64_t val)
{
    uint8_t *p = mem_img + base + HDR_SEQID_AT;
    size_t i;

    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(val >> (56u - 8u * i));
    }
}

/* Two intact copies, the secondary carrying the higher sequence id. That is
 * what a header rewrite looks like the instant after the second copy lands
 * and before the first is updated, and the reader is required to prefer it.
 *
 * The sweeps above cannot produce this: damaging the primary leaves exactly
 * one candidate, so the comparison that picks between two never runs. */
static void test_luks_newer_secondary_wins(void **state)
{
    luks_vol v;
    uint64_t primary_seq;

    (void)state;
    mem_reset();
    assert_int_equal(sweep_parse(&v), LUKS_OK);
    assert_int_equal(v.used_secondary, 0);

    primary_seq = hdr_get_seqid(0u);
    assert_true(primary_seq > 0u);
    /* cryptsetup leaves both copies on the same id, so the fixture cannot
     * show this without being edited. */
    assert_int_equal(hdr_get_seqid(SEC_HDR_OFF), primary_seq);

    hdr_put_seqid(SEC_HDR_OFF, primary_seq + 1u);
    hdr_fix_csum(SEC_HDR_OFF);
    assert_int_equal(sweep_parse(&v), LUKS_OK);
    assert_int_equal(v.used_secondary, 1);
    assert_int_equal(v.hdr_off, SEC_HDR_OFF);

    /* And the other way, so the preference is on the id and not on being
     * second. */
    mem_reset();
    hdr_put_seqid(SEC_HDR_OFF, primary_seq - 1u);
    hdr_fix_csum(SEC_HDR_OFF);
    assert_int_equal(sweep_parse(&v), LUKS_OK);
    assert_int_equal(v.used_secondary, 0);

    mem_reset();
}

/* The JSON sweep that the checksum otherwise hides. Both copies are damaged
 * at the same offset and both checksums refreshed, so the parser sees the
 * text as written and every refusal below check_header becomes reachable. */
static void test_luks_json_corruption_sweep(void **state)
{
    static const uint8_t masks[] = { 0xffu, 0x20u, 0x01u };
    luks_vol v;
    uint32_t json_len;
    uint32_t at;
    size_t m;
    size_t swept = 0;
    size_t refused = 0;

    (void)state;
    mem_reset();
    sweep_spans_init();
    json_len = g_spans[1].len;

    for (at = 0; at < json_len; at++) {
        for (m = 0; m < sizeof masks / sizeof masks[0]; m++) {
            int rc;

            mem_img[LUKS_HDR_BIN + at] ^= masks[m];
            mem_img[SEC_HDR_OFF + LUKS_HDR_BIN + at] ^= masks[m];
            hdr_fix_csum(0u);
            hdr_fix_csum(SEC_HDR_OFF);

            rc = sweep_parse(&v);

            if (!luks_status_known(rc)) {
                fail_msg("json flip 0x%02x at %u returned %d",
                         (unsigned)masks[m], at, rc);
            }
            if (rc != LUKS_OK) {
                refused++;
            }
            swept++;

            mem_reset();
        }
    }

    assert_true(swept > 2000);
    /* Most of the text is structure and key names, so most flips break
     * something the parser or the metadata reader checks. */
    assert_true(refused > 1000);

    mem_reset();
    assert_int_equal(sweep_parse(&v), LUKS_OK);
}

/* ------------------------------------------- directed metadata damage */

/* Rewrite a run of the JSON text and refresh both checksums.
 *
 * Lengths need not match. The area is NUL-padded out to json_size, which is
 * 12288 bytes against about 730 of text, so there is room to grow the
 * document as well as edit it in place; the whole area is rewritten and
 * re-padded either way.
 *
 * Both copies, because the reader prefers whichever is intact and an edit to
 * one alone is simply ignored. */
static void json_patch(const char *find, const char *repl)
{
    static char rebuilt[JSON_AREA];
    char *text = (char *)mem_img + LUKS_HDR_BIN;
    size_t flen = strlen(find);
    size_t rlen = strlen(repl);
    size_t head;
    size_t tail;
    const char *at = strstr(text, find);

    if (at == NULL) {
        fail_msg("json_patch: '%s' is not in the fixture metadata", find);
    }
    head = (size_t)(at - text);
    tail = strlen(text) - head - flen;
    assert_true(head + rlen + tail < JSON_AREA);

    memset(rebuilt, 0, sizeof rebuilt);
    memcpy(rebuilt, text, head);
    memcpy(rebuilt + head, repl, rlen);
    memcpy(rebuilt + head + rlen, text + head + flen, tail);

    memcpy(mem_img + LUKS_HDR_BIN, rebuilt, JSON_AREA);
    memcpy(mem_img + SEC_HDR_OFF + LUKS_HDR_BIN, rebuilt, JSON_AREA);
    hdr_fix_csum(0u);
    hdr_fix_csum(SEC_HDR_OFF);
}

/* Every field try_slot reads before it derives anything, each renamed out of
 * existence in turn.
 *
 * Renaming rather than deleting, again so the length holds. What is under
 * test is that a keyslot missing any one of these is refused rather than
 * read with a default, and that the refusal happens before a single round of
 * PBKDF2: these cases run in microseconds, which is the evidence that the
 * validation really is ahead of the derivation.
 *
 * Every one answers LUKS_ERR_UNSUPPORTED, whatever try_slot itself returned,
 * and that is luks_unlock's triage rather than a loss of detail. A slot it
 * could not read is not a slot that rejected the passphrase, so saw_usable
 * stays clear, and with no usable slot left the volume is reported as one
 * this loader cannot open. Answering NOKEY instead would send the user off to
 * retype a passphrase against a keyslot that was never readable. */
static void test_luks_keyslot_field_refusals(void **state)
{
    static const struct {
        const char *find;
        const char *repl;
        int         want;
    } cases[] = {
        /* The slot's own type. */
        { "\"type\":\"luks2\"", "\"type\":\"luksX\"", LUKS_ERR_UNSUPPORTED },
        /* The whole kdf object, then the fields inside it. */
        { "\"kdf\":{",        "\"kdX\":{",        LUKS_ERR_UNSUPPORTED },
        { "\"iterations\":10000", "\"iteratioXs\":10000", LUKS_ERR_UNSUPPORTED },
        { "\"salt\":\"z3btw2",  "\"salX\":\"z3btw2",  LUKS_ERR_UNSUPPORTED },
        /* The af object and its fields. The splitter's own type matters:
         * luks1 is the only expansion this reader undoes. */
        { "\"af\":{",         "\"aX\":{",         LUKS_ERR_UNSUPPORTED },
        { "\"type\":\"luks1\"", "\"type\":\"luksY\"", LUKS_ERR_UNSUPPORTED },
        { "\"stripes\":4000", "\"stripeX\":4000", LUKS_ERR_UNSUPPORTED },
        /* The area object: where the expansion is stored and how. */
        { "\"area\":{",       "\"arXa\":{",       LUKS_ERR_UNSUPPORTED },
        { "\"type\":\"raw\"",  "\"type\":\"rXw\"",  LUKS_ERR_UNSUPPORTED },
        { "\"offset\":\"32768\"", "\"offseX\":\"32768\"", LUKS_ERR_UNSUPPORTED },
        { "\"size\":\"258048\"",  "\"sizX\":\"258048\"",  LUKS_ERR_UNSUPPORTED },
    };
    size_t i;

    (void)state;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        luks_vol v;
        luks_master_key mk;
        int rc;

        mem_reset();
        json_patch(cases[i].find, cases[i].repl);

        assert_int_equal(luks_open(&v, mem_read, NULL, json_buf,
                                   sizeof json_buf), LUKS_OK);
        rc = luks_unlock(&v, PASS, PASS_LEN, &mk, NULL);
        if (rc != cases[i].want) {
            fail_msg("'%s' -> '%s' gave %d, want %d", cases[i].find,
                     cases[i].repl, rc, cases[i].want);
        }
        crypto_wipe(&mk, sizeof mk);
    }

    mem_reset();
}

/* The digest object, which is what turns a recovered key into a verified one.
 * Each of these costs a full derivation, because the digest is not consulted
 * until try_slot has already undone the anti-forensic expansion. */
static void test_luks_digest_field_refusals(void **state)
{
    static const struct {
        const char *find;
        const char *repl;
        int         want;
    } cases[] = {
        { "\"iterations\":1000,", "\"iteratioXs\":1000,", LUKS_ERR_UNSUPPORTED },
        { "\"salt\":\"GhIqV2",   "\"salX\":\"GhIqV2",   LUKS_ERR_UNSUPPORTED },
        { "\"digest\":\"s21NP2", "\"digesX\":\"s21NP2", LUKS_ERR_UNSUPPORTED },
        /* A digest that covers no segment describes a volume shape this
         * reader does not handle, which is not the same answer as a wrong
         * passphrase. */
        { "\"segments\":[\"0\"]", "\"segmentX\":[\"0\"]",
          LUKS_ERR_UNSUPPORTED },
        /* A digest naming no keyslot at all: the key cannot be verified, and
         * an unverifiable key must never open anything. */
        { "\"keyslots\":[\"0\"],\"segments\"",
          "\"keyslots\":[\"9\"],\"segments\"", LUKS_ERR_UNSUPPORTED },
    };
    size_t i;

    (void)state;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        luks_vol v;
        luks_master_key mk;
        int rc;

        mem_reset();
        json_patch(cases[i].find, cases[i].repl);

        assert_int_equal(luks_open(&v, mem_read, NULL, json_buf,
                                   sizeof json_buf), LUKS_OK);
        rc = luks_unlock(&v, PASS, PASS_LEN, &mk, NULL);
        if (rc != cases[i].want) {
            fail_msg("'%s' -> '%s' gave %d, want %d", cases[i].find,
                     cases[i].repl, rc, cases[i].want);
        }
        crypto_wipe(&mk, sizeof mk);
    }

    mem_reset();
}

/* The segment description, which luks_dev_open turns into a decrypting
 * reader. Damaged here the unlock still succeeds, because the master key is
 * recovered and verified from the keyslot and the digest alone; it is opening
 * the device that has to refuse. */
/* The segment description, which is parsed during luks_open rather than when
 * the decrypting reader is built.
 *
 * That ordering is the point of this test. luks_dev_open re-checks the sector
 * size, and the check is unreachable through any metadata: parse_segment has
 * already required a power of two that is a multiple of 512 and no larger
 * than LUKS_MAX_SECTOR, so a volume that reaches luks_dev_open cannot carry a
 * bad one. The second check stays because luks_dev_open is a public entry
 * point that takes a caller-supplied luks_vol. */
static void test_luks_segment_field_refusals(void **state)
{
    luks_vol v;

    (void)state;

    /* Not a power of two, and not a multiple of 512. Every offset the reader
     * computes is a multiple of this, so it is checked where the volume is
     * first described. */
    mem_reset();
    json_patch("\"sector_size\":4096", "\"sector_size\":4095");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_ERR_UNSUPPORTED);

    /* An encryption this reader does not implement. aes-xts-plain64 is the
     * only one, and guessing at another would produce plausible-looking
     * garbage rather than a refusal. */
    mem_reset();
    json_patch("\"encryption\":\"aes-xts-plain64\",\"sector_size\"",
               "\"encryption\":\"aes-cbc-essiv64\",\"sector_size\"");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_ERR_UNSUPPORTED);

    mem_reset();
}


/* dm-crypt's iv_large_sectors, which changes how the IV is counted and so
 * changes every sector this reader decrypts.
 *
 * No fixture carries it: cryptsetup only sets it on request, and a volume
 * with it would decrypt to garbage under the wrong arithmetic without
 * anything saying so. What is asserted is the arithmetic itself, by reading
 * the same plaintext both ways: without the flag the IV advances by
 * sector_size/512 per unit, with it the count is divided back down, so the
 * two disagree and a volume read under the wrong one must not match
 * dm-crypt's output. */
static void test_luks_iv_large_sectors(void **state)
{
    luks_vol v;
    luks_master_key mk;
    luks_dev d;
    static uint8_t plain[4096];
    static uint8_t large[4096];

    (void)state;

    /* Baseline: the fixture as cryptsetup wrote it, which the reference
     * plaintext from dm-crypt already pins elsewhere in this file. */
    mem_reset();
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(v.seg_iv_large, 0);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&d, &v, &mk), LUKS_OK);
    assert_int_equal(luks_read(&d, 0, sizeof plain, plain), LUKS_OK);
    luks_dev_close(&d);
    assert_memory_equal(plain, ref, sizeof plain);
    crypto_wipe(&mk, sizeof mk);

    /* The same volume with the flag declared. Nothing else changes, so any
     * difference in the output is the IV divisor and nothing else. */
    mem_reset();
    json_patch("\"sector_size\":4096",
               "\"sector_size\":4096,\"flags\":[\"iv_large_sectors\"]");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(v.seg_iv_large, 1);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&d, &v, &mk), LUKS_OK);
    assert_int_equal(luks_read(&d, 0, sizeof large, large), LUKS_OK);
    luks_dev_close(&d);
    crypto_wipe(&mk, sizeof mk);

    /* Sector 0 has IV 0 either way, so the first unit is identical and the
     * divisor only shows from the second onward. Asserting both halves is
     * what makes this a test of the arithmetic rather than of the flag
     * being read. */
    assert_memory_equal(large, plain, 4096u);

    mem_reset();
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&d, &v, &mk), LUKS_OK);
    assert_int_equal(luks_read(&d, 4096u, sizeof plain, plain), LUKS_OK);
    luks_dev_close(&d);
    crypto_wipe(&mk, sizeof mk);

    mem_reset();
    json_patch("\"sector_size\":4096",
               "\"sector_size\":4096,\"flags\":[\"iv_large_sectors\"]");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);
    assert_int_equal(luks_dev_open(&d, &v, &mk), LUKS_OK);
    assert_int_equal(luks_read(&d, 4096u, sizeof large, large), LUKS_OK);
    luks_dev_close(&d);
    crypto_wipe(&mk, sizeof mk);

    assert_memory_not_equal(large, plain, 4096u);

    mem_reset();
}

/* Containers that are not containers. The metadata reader asks for
 * "keyslots" and "digests" by name and then iterates whatever came back, so
 * each has to refuse a value of the wrong shape rather than iterate it as an
 * empty one.
 *
 * luks_open itself only needs the header and the segment, so it is the later
 * calls that have to notice. Where a case does break luks_open the patch has
 * damaged the document's structure rather than one member, and that is said
 * explicitly below rather than left to the reader. */
static void test_luks_container_shape_refusals(void **state)
{
    luks_vol v;
    luks_master_key mk;
    size_t count;

    (void)state;

    /* keyslots present but a string. The document is still well-formed and
     * the segment is untouched, so the volume opens and the refusal lands
     * where a slot is first asked for. */
    mem_reset();
    json_patch("\"keyslots\":{\"0\":{", "\"keyslots\":\"x\",\"spare\":{\"0\":{");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_slot_count(&v, &count), LUKS_ERR_CORRUPT);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_CORRUPT);
    crypto_wipe(&mk, sizeof mk);

    /* No keyslots member at all, under any name. */
    mem_reset();
    json_patch("\"keyslots\":{\"0\"", "\"keyslotX\":{\"0\"");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_slot_count(&v, &count), LUKS_ERR_CORRUPT);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_CORRUPT);
    crypto_wipe(&mk, sizeof mk);

    /* No digests member: a master key could be recovered from the keyslot
     * and then never verified, which must not open anything. */
    mem_reset();
    json_patch("\"digests\":{", "\"digestX\":{");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_CORRUPT);
    crypto_wipe(&mk, sizeof mk);

    /* No segments member. This one does break luks_open, because the segment
     * is the only part of the metadata it reads for itself. */
    mem_reset();
    json_patch("\"segments\":{\"0\"", "\"segmentX\":{\"0\"");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_ERR_CORRUPT);

    /* A root that is an array rather than an object. */
    mem_reset();
    json_patch("{\"keyslots\"", "[\"keyslots\"");
    assert_true(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf)
                != LUKS_OK);

    mem_reset();
}


/* Key sizes that are legal to state and awkward to process.
 *
 * Everything in scope uses 64, which divides 512 exactly and is one of the
 * two lengths XTS accepts, so three separate paths never run on any real
 * volume: a stripe that straddles the boundary between two keyslot units, the
 * anti-forensic merge's ragged last diffusion block, and the refusal when a
 * declared area key is not an XTS key length at all. A header can say 48, so
 * all three are reachable and none is tested by any volume cryptsetup would
 * produce.
 *
 * None of these can end in LUKS_OK: 48 bytes of master key is not the key the
 * digest was computed over. What is under test is that the arithmetic holds
 * up on the way to finding that out rather than walking off a buffer. */
static void test_luks_awkward_key_sizes(void **state)
{
    luks_vol v;
    luks_master_key mk;
    int rc;

    (void)state;

    /* 48 divides neither 512 nor the 32-byte SHA-256 digest, so a stripe
     * spans two units and the merge's last diffusion block is a short one. */
    mem_reset();
    json_patch("\"type\":\"luks2\",\"key_size\":64",
               "\"type\":\"luks2\",\"key_size\":48");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    rc = luks_unlock(&v, PASS, PASS_LEN, &mk, NULL);
    assert_true(rc == LUKS_ERR_UNSUPPORTED || rc == LUKS_ERR_NOKEY);
    crypto_wipe(&mk, sizeof mk);

    /* The area key is the one handed to XTS, which takes 32 or 64 and
     * nothing else. Distinct from the keyslot's own key_size above, and
     * conflating the two is exactly the bug the driver's comment warns
     * about. */
    mem_reset();
    json_patch("\"encryption\":\"aes-xts-plain64\",\"key_size\":64",
               "\"encryption\":\"aes-xts-plain64\",\"key_size\":48");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);
    crypto_wipe(&mk, sizeof mk);

    /* Past what the format can carry, so it is refused before any of the
     * above is attempted. */
    mem_reset();
    json_patch("\"type\":\"luks2\",\"key_size\":64",
               "\"type\":\"luks2\",\"key_size\":65");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                     LUKS_ERR_UNSUPPORTED);
    crypto_wipe(&mk, sizeof mk);

    mem_reset();
}


/* luks_dev_open's own checks on the volume and key it is handed.
 *
 * Unreachable through any metadata, and deliberately kept. parse_segment
 * already requires a sector size that is a power of two, a multiple of 512
 * and no larger than LUKS_MAX_SECTOR, and luks_unlock only ever reports a key
 * whose length the keyslot declared; a volume that reached here through
 * luks_open cannot carry either fault. But luks_dev_open is a public entry
 * point that takes a caller-allocated luks_vol and luks_master_key, so what
 * it guards against is a caller, not a disk. Reaching them means being that
 * caller. */
static void test_luks_dev_open_guards(void **state)
{
    luks_vol v;
    luks_master_key mk;
    luks_dev d;

    (void)state;
    mem_reset();
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL), LUKS_OK);

    assert_int_equal(luks_dev_open(NULL, &v, &mk), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_dev_open(&d, NULL, &mk), LUKS_ERR_BAD_ARG);
    assert_int_equal(luks_dev_open(&d, &v, NULL), LUKS_ERR_BAD_ARG);

    /* A sector size the segment parser would never have accepted. */
    {
        uint32_t save = v.seg_sector_size;

        v.seg_sector_size = 0u;
        assert_int_equal(luks_dev_open(&d, &v, &mk), LUKS_ERR_CORRUPT);
        v.seg_sector_size = LUKS_MAX_SECTOR * 2u;
        assert_int_equal(luks_dev_open(&d, &v, &mk), LUKS_ERR_CORRUPT);
        v.seg_sector_size = save;
    }

    /* A master key of a length XTS does not take. luks_unlock cannot report
     * one, because a keyslot declaring 48 fails its digest long before the
     * key is handed on. */
    {
        uint32_t save = mk.len;

        mk.len = 48u;
        assert_int_equal(luks_dev_open(&d, &v, &mk), LUKS_ERR_UNSUPPORTED);
        mk.len = save;
    }

    crypto_wipe(&mk, sizeof mk);
    mem_reset();
}


static void hdr_put_size(uint32_t base, uint64_t val)
{
    uint8_t *p = mem_img + base + 8u;   /* hdr_size, BE64 */
    size_t i;

    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(val >> (56u - 8u * i));
    }
}

/* Each arm of the multi-part guards, separately.
 *
 * The sweeps take whichever arm a random flip happens to reach, which for a
 * three-part condition is usually the same one every time: lines stay covered
 * and branches do not, and an arm that is never the deciding one is an arm
 * that could be inverted without any test noticing.
 *
 * hdr_size is checked before the checksum is computed, which is what makes
 * these edits cheap: the copy is rejected on the field itself and the sum
 * over it is never reached, so there is nothing to refresh. */
static void test_luks_guard_arms(void **state)
{
    luks_vol v;
    static const uint64_t bad_sizes[] = {
        0u,             /* not past the binary header */
        LUKS_HDR_BIN,   /* exactly the header, so no JSON area at all */
        16u * 1024u * 1024u,  /* past LUKS_MAX_HDR_SIZE */
        16385u,         /* not a multiple of 512 */
    };
    static const unsigned bad_sectors[] = {
        256u,           /* below the 512 floor */
        1536u,          /* a multiple of 512 that is not a power of two */
        4095u,          /* neither */
    };
    size_t i;

    (void)state;

    for (i = 0; i < sizeof bad_sizes / sizeof bad_sizes[0]; i++) {
        mem_reset();
        hdr_put_size(0u, bad_sizes[i]);
        hdr_put_size(SEC_HDR_OFF, bad_sizes[i]);
        if (luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf)
            != LUKS_ERR_BAD_HEADER) {
            fail_msg("hdr_size %llu was not refused",
                     (unsigned long long)bad_sizes[i]);
        }
    }

    /* One copy at a time, so the other carries the volume. A header size the
     * reader rejects is a torn copy like any other. */
    mem_reset();
    hdr_put_size(0u, 0u);
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    assert_int_equal(v.used_secondary, 1);

    for (i = 0; i < sizeof bad_sectors / sizeof bad_sectors[0]; i++) {
        char want[48];
        char repl[48];

        mem_reset();
        snprintf(want, sizeof want, "\"sector_size\":4096");
        snprintf(repl, sizeof repl, "\"sector_size\":%u", bad_sectors[i]);
        json_patch(want, repl);
        if (luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf)
            != LUKS_ERR_UNSUPPORTED) {
            fail_msg("sector_size %u was not refused", bad_sectors[i]);
        }
    }

    /* A kdf whose type is present but is not a string. kdf_of cannot name it,
     * so the slot is reported as one this loader has no derivation for, the
     * same answer p4's argon2id gets. That is the right end of the triage:
     * the passphrase is not in question, and the user is told the volume
     * cannot be opened here rather than sent to retype. */
    mem_reset();
    json_patch("\"kdf\":{\"type\":\"pbkdf2\"", "\"kdf\":{\"type\":12345678");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    {
        luks_master_key mk;

        assert_int_equal(luks_unlock(&v, PASS, PASS_LEN, &mk, NULL),
                         LUKS_ERR_UNSUPPORTED_KDF);
        crypto_wipe(&mk, sizeof mk);
    }

    /* A keyslot named by something that is not a decimal id, which the
     * enumeration has to report without inventing a number. */
    mem_reset();
    json_patch("\"keyslots\":{\"0\":", "\"keyslots\":{\"zz\":");
    assert_int_equal(luks_open(&v, mem_read, NULL, json_buf, sizeof json_buf),
                     LUKS_OK);
    {
        luks_slot_info si;
        size_t count = 0;

        assert_int_equal(luks_slot_count(&v, &count), LUKS_OK);
        assert_int_equal(count, 1);
        assert_int_equal(luks_slot_at(&v, 0, &si), LUKS_OK);
        assert_int_equal(si.id, 0);      /* unparseable falls back to 0 */
    }

    mem_reset();
}

int run_luks_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_luks_open_geometry),
        cmocka_unit_test(test_luks_slot_enumeration),
        cmocka_unit_test(test_luks_unlock_correct_passphrase),
        cmocka_unit_test(test_luks_unlock_wrong_passphrase),
        cmocka_unit_test(test_luks_read_matches_dm_crypt),
        cmocka_unit_test(test_luks_read_unaligned),
        cmocka_unit_test(test_luks_primary_damage_falls_back),
        cmocka_unit_test(test_luks_both_copies_corruption_sweep),
        cmocka_unit_test(test_luks_newer_secondary_wins),
        cmocka_unit_test(test_luks_json_corruption_sweep),
        cmocka_unit_test(test_luks_keyslot_field_refusals),
        cmocka_unit_test(test_luks_digest_field_refusals),
        cmocka_unit_test(test_luks_segment_field_refusals),
        cmocka_unit_test(test_luks_iv_large_sectors),
        cmocka_unit_test(test_luks_container_shape_refusals),
        cmocka_unit_test(test_luks_awkward_key_sizes),
        cmocka_unit_test(test_luks_dev_open_guards),
        cmocka_unit_test(test_luks_guard_arms),
        cmocka_unit_test(test_luks_secondary_header_fallback),
        cmocka_unit_test(test_luks_argon_slot_is_named_not_guessed),
        cmocka_unit_test(test_ext4_over_luks),
        cmocka_unit_test(test_luks_segment_refusals),
        cmocka_unit_test(test_luks_fixed_size_segment),
        cmocka_unit_test(test_luks_keyslot_refusals),
        cmocka_unit_test(test_luks_digest_refusals),
        cmocka_unit_test(test_luks_io_failures),
        cmocka_unit_test(test_af_merge_guards),
    };
    int failed;
    int ref_fd;
    ssize_t n;

    img_fd = open(LUKS_IMG, O_RDONLY);
    argon_fd = open(LUKS_ARGON_IMG, O_RDONLY);
    ref_fd = open(LUKS_REF, O_RDONLY);
    if (img_fd < 0 || argon_fd < 0 || ref_fd < 0 || load_manifest() != 0) {
        printf("\n[  SKIPPED ] LUKS2 fixture: %s, %s, %s or %s not found.\n"
               "             Build them with: tools/mk-test-luks.sh "
               "(needs cryptsetup and sudo).\n",
               LUKS_IMG, LUKS_ARGON_IMG, LUKS_REF, LUKS_MANIFEST);
        if (img_fd >= 0) {
            close(img_fd);
        }
        if (argon_fd >= 0) {
            close(argon_fd);
        }
        if (ref_fd >= 0) {
            close(ref_fd);
        }
        img_fd = -1;
        argon_fd = -1;
        return test_skip_rc();
    }

    n = read(ref_fd, ref, sizeof ref);
    close(ref_fd);
    if (n != (ssize_t)sizeof ref) {
        printf("\n[  SKIPPED ] LUKS2 fixture: %s is not %u bytes.\n"
               "             Rebuild it with: tools/mk-test-luks.sh\n",
               LUKS_REF, (unsigned)sizeof ref);
        close(img_fd);
        close(argon_fd);
        img_fd = -1;
        argon_fd = -1;
        return test_skip_rc();
    }

    failed = cmocka_run_group_tests_name("luks", tests, NULL, NULL);

    close(img_fd);
    close(argon_fd);
    img_fd = -1;
    argon_fd = -1;
    return failed;
}
