/* airies-boot: read-only LUKS2 unlock. See luks.h.
 *
 * The binary header is 4096 bytes of big-endian fields followed by the JSON
 * metadata area, and the whole of it (header plus area, with the 64-byte
 * checksum field zeroed) hashes to the checksum the header carries. Two
 * copies exist, the second immediately after the first, so a header rewrite
 * interrupted by a power cut leaves one intact; both are validated here and
 * the higher sequence id wins, which is the rule cryptsetup uses.
 *
 * Field offsets, verified against cryptsetup 2.8.7 output rather than taken
 * from memory:
 *     0   magic[6]        "LUKS\xba\xbe" primary, "SKUL\xba\xbe" secondary
 *     6   version         BE16, 2
 *     8   hdr_size        BE64, binary header + JSON area
 *    16   seqid           BE64
 *    24   label[48]
 *    72   checksum_alg[32]
 *   104   salt[64]
 *   168   uuid[40]
 *   208   subsystem[48]
 *   256   hdr_offset      BE64, this copy's own device offset
 *   264   padding[184]
 *   448   csum[64]
 *   512   padding to 4096
 *  4096   JSON text, NUL-padded to hdr_size
 */

#include <luks/luks.h>
#include <luks/json.h>
#include <luks/af.h>
#include <crypto/crypto.h>
#include <crypto/hash.h>
#include <crypto/pbkdf2.h>
#include <crypto/xts.h>
#include <lib/string.h>

#define HDR_MAGIC_LEN     6u
#define HDR_OFF_VERSION   6u
#define HDR_OFF_HDR_SIZE  8u
#define HDR_OFF_SEQID     16u
#define HDR_OFF_CSUM_ALG  72u
#define HDR_OFF_HDR_OFF   256u
#define HDR_OFF_CSUM      448u
#define HDR_CSUM_FIELD    64u

#define LUKS_MAX_HDR_SIZE (4u * 1024u * 1024u)
#define LUKS_MAX_STRIPES  8192u

static const uint8_t magic_primary[HDR_MAGIC_LEN] = {
    'L', 'U', 'K', 'S', 0xbau, 0xbeu
};
static const uint8_t magic_secondary[HDR_MAGIC_LEN] = {
    'S', 'K', 'U', 'L', 0xbau, 0xbeu
};

/* Where a secondary header can legally sit, per cryptsetup's fixed ladder
 * of metadata sizes. Scanned only when the primary is unreadable, which is
 * the case the second copy exists for. */
static const uint64_t secondary_offsets[] = {
    0x004000ull, 0x008000ull, 0x010000ull, 0x020000ull, 0x040000ull,
    0x080000ull, 0x100000ull, 0x200000ull, 0x400000ull
};

static uint64_t be64_at(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static uint16_t be16_at(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Length of a NUL-terminated field that need not be terminated within its
 * fixed width. */
static size_t field_len(const uint8_t *p, size_t cap)
{
    size_t n = 0;

    while (n < cap && p[n] != '\0') {
        n++;
    }
    return n;
}

typedef struct {
    uint64_t off;
    uint64_t hdr_size;
    uint64_t seqid;
} hdr_candidate;

/* Reads the binary header at off, checks its self-consistent fields, then
 * reads the JSON area into json_buf and verifies the checksum over the
 * whole hdr_size with the checksum field zeroed. */
static int check_header(luks_vol *v, uint64_t off, int expect_secondary,
                        char *json_buf, size_t json_cap, hdr_candidate *out)
{
    uint8_t raw[LUKS_HDR_BIN];
    uint8_t want[HDR_CSUM_FIELD];
    uint8_t got[HASH_MAX_DIGEST];
    const uint8_t *magic = expect_secondary ? magic_secondary : magic_primary;
    const hash_desc *h;
    hash_ctx hc;
    uint64_t hdr_size;
    size_t alg_len;
    size_t area;

    if (v->read(v->read_ctx, off, LUKS_HDR_BIN, raw) != 0) {
        return LUKS_ERR_IO;
    }
    if (memcmp(raw, magic, HDR_MAGIC_LEN) != 0) {
        return LUKS_ERR_BAD_HEADER;
    }
    if (be16_at(raw + HDR_OFF_VERSION) != 2u) {
        return LUKS_ERR_BAD_HEADER;
    }
    hdr_size = be64_at(raw + HDR_OFF_HDR_SIZE);
    if (hdr_size <= LUKS_HDR_BIN || hdr_size > LUKS_MAX_HDR_SIZE ||
        (hdr_size % 512u) != 0u) {
        return LUKS_ERR_BAD_HEADER;
    }
    /* A copy that disagrees about where it lives is not this copy. */
    if (be64_at(raw + HDR_OFF_HDR_OFF) != off) {
        return LUKS_ERR_BAD_HEADER;
    }

    alg_len = field_len(raw + HDR_OFF_CSUM_ALG, 32u);
    h = hash_by_name((const char *)(raw + HDR_OFF_CSUM_ALG), alg_len);
    if (h == NULL) {
        return LUKS_ERR_UNSUPPORTED;
    }

    area = (size_t)(hdr_size - LUKS_HDR_BIN);
    if (area > json_cap) {
        return LUKS_ERR_NOSPACE;
    }
    if (v->read(v->read_ctx, off + LUKS_HDR_BIN, area, json_buf) != 0) {
        return LUKS_ERR_IO;
    }

    memcpy(want, raw + HDR_OFF_CSUM, HDR_CSUM_FIELD);
    memset(raw + HDR_OFF_CSUM, 0, HDR_CSUM_FIELD);

    hash_init(&hc, h);
    hash_update(&hc, raw, LUKS_HDR_BIN);
    hash_update(&hc, json_buf, area);
    hash_final(&hc, got);
    if (memcmp(got, want, h->digest_len) != 0) {
        return LUKS_ERR_BAD_HEADER;
    }

    out->off = off;
    out->hdr_size = hdr_size;
    out->seqid = be64_at(raw + HDR_OFF_SEQID);
    return LUKS_OK;
}

static int load_json(luks_vol *v, const hdr_candidate *c,
                     char *json_buf, size_t json_cap)
{
    size_t area = (size_t)(c->hdr_size - LUKS_HDR_BIN);
    size_t text;

    if (area >= json_cap) {
        return LUKS_ERR_NOSPACE;
    }
    if (v->read(v->read_ctx, c->off + LUKS_HDR_BIN, area, json_buf) != 0) {
        return LUKS_ERR_IO;
    }
    json_buf[area] = '\0';
    text = field_len((const uint8_t *)json_buf, area);

    v->json = json_buf;
    v->json_len = text;
    v->json_area = area;
    v->hdr_off = c->off;
    v->hdr_size = c->hdr_size;
    v->seqid = c->seqid;
    return LUKS_OK;
}

/* ---- metadata helpers ------------------------------------------------ */

static int obj_get(const json_value *o, const char *name, json_value *out)
{
    size_t n = 0;

    while (name[n] != '\0') {
        n++;
    }
    if (json_obj_get(o, name, n, out) != JSON_OK) {
        return LUKS_ERR_CORRUPT;
    }
    return LUKS_OK;
}

static int obj_u64(const json_value *o, const char *name, uint64_t *out)
{
    json_value v;
    int rc = obj_get(o, name, &v);

    if (rc != LUKS_OK) {
        return rc;
    }
    return (json_u64(&v, out) == JSON_OK) ? LUKS_OK : LUKS_ERR_CORRUPT;
}

static int obj_u32(const json_value *o, const char *name, uint32_t max,
                   uint32_t *out)
{
    uint64_t big;
    int rc = obj_u64(o, name, &big);

    if (rc != LUKS_OK) {
        return rc;
    }
    if (big > (uint64_t)max) {
        return LUKS_ERR_UNSUPPORTED;
    }
    *out = (uint32_t)big;
    return LUKS_OK;
}

static int obj_hash(const json_value *o, const char *name,
                    const hash_desc **out)
{
    json_value v;
    int rc = obj_get(o, name, &v);

    if (rc != LUKS_OK) {
        return rc;
    }
    if (v.type != JSON_STRING) {
        return LUKS_ERR_CORRUPT;
    }
    *out = hash_by_name(v.ptr, v.len);
    return (*out != NULL) ? LUKS_OK : LUKS_ERR_UNSUPPORTED;
}

static int obj_b64(const json_value *o, const char *name,
                   uint8_t *out, size_t cap, size_t *out_len)
{
    json_value v;
    int rc = obj_get(o, name, &v);

    if (rc != LUKS_OK) {
        return rc;
    }
    rc = json_b64(&v, out, cap, out_len);
    if (rc == JSON_ERR_NOSPACE) {
        return LUKS_ERR_UNSUPPORTED;
    }
    return (rc == JSON_OK) ? LUKS_OK : LUKS_ERR_CORRUPT;
}

static int obj_str_is(const json_value *o, const char *name, const char *want)
{
    json_value v;

    if (obj_get(o, name, &v) != LUKS_OK) {
        return 0;
    }
    return json_str_eq(&v, want);
}

static int arr_has_str(const json_value *arr, const char *s, size_t slen)
{
    json_iter it;
    json_value e;

    if (arr->type != JSON_ARRAY || json_iter_init(arr, &it) != JSON_OK) {
        return 0;
    }
    for (;;) {
        int rc = json_iter_next(&it, NULL, &e);

        if (rc <= 0) {
            return 0;
        }
        if (e.type == JSON_STRING && e.len == slen &&
            memcmp(e.ptr, s, slen) == 0) {
            return 1;
        }
    }
}

static luks_kdf_type kdf_of(const json_value *kdf)
{
    json_value t;

    if (obj_get(kdf, "type", &t) != LUKS_OK || t.type != JSON_STRING) {
        return LUKS_KDF_UNKNOWN;
    }
    if (json_str_eq(&t, "pbkdf2")) {
        return LUKS_KDF_PBKDF2;
    }
    if (json_str_eq(&t, "argon2id")) {
        return LUKS_KDF_ARGON2ID;
    }
    if (json_str_eq(&t, "argon2i")) {
        return LUKS_KDF_ARGON2I;
    }
    return LUKS_KDF_UNKNOWN;
}

static int root_of(const luks_vol *v, json_value *root)
{
    if (json_parse(v->json, v->json_len, root) != JSON_OK ||
        root->type != JSON_OBJECT) {
        return LUKS_ERR_CORRUPT;
    }
    return LUKS_OK;
}

/* ---- segment --------------------------------------------------------- */

static int parse_segment(luks_vol *v, const json_value *root)
{
    json_value segs, seg, flags;
    uint64_t off, tweak;
    uint32_t ss;
    int rc;

    rc = obj_get(root, "segments", &segs);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_get(&segs, "0", &seg);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (!obj_str_is(&seg, "type", "crypt")) {
        return LUKS_ERR_UNSUPPORTED;
    }
    /* DESIGN Section 3 fixes p3's cipher; anything else is a volume this
     * loader was not built to read, and saying so beats deriving a key that
     * produces garbage. */
    if (!obj_str_is(&seg, "encryption", "aes-xts-plain64")) {
        return LUKS_ERR_UNSUPPORTED;
    }
    rc = obj_u64(&seg, "offset", &off);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_u32(&seg, "sector_size", LUKS_MAX_SECTOR, &ss);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (ss < 512u || (ss % 512u) != 0u || (ss & (ss - 1u)) != 0u) {
        return LUKS_ERR_UNSUPPORTED;
    }
    rc = obj_u64(&seg, "iv_tweak", &tweak);
    if (rc != LUKS_OK) {
        return rc;
    }

    v->seg_offset = off;
    v->seg_sector_size = ss;
    v->seg_iv_tweak = tweak;
    v->seg_dynamic = 0;
    v->seg_size = 0;
    v->seg_iv_large = 0;

    {
        json_value sz;

        rc = obj_get(&seg, "size", &sz);
        if (rc != LUKS_OK) {
            return rc;
        }
        if (json_str_eq(&sz, "dynamic")) {
            v->seg_dynamic = 1;
        } else if (json_u64(&sz, &v->seg_size) != JSON_OK) {
            return LUKS_ERR_CORRUPT;
        }
    }

    /* iv_large_sectors changes how dm-crypt counts the IV, so it has to be
     * honoured rather than ignored: on a 4096-byte volume the difference
     * shows up on every block but the first. */
    if (json_obj_get(&seg, "flags", 5u, &flags) == JSON_OK) {
        if (arr_has_str(&flags, "iv_large_sectors", 16u)) {
            v->seg_iv_large = 1;
        }
    }
    return LUKS_OK;
}

/* ---- open ------------------------------------------------------------ */

int luks_open(luks_vol *v, luks_read_fn read, void *read_ctx,
              char *json_buf, size_t json_cap)
{
    hdr_candidate cand[2];
    hdr_candidate best;
    json_value root;
    size_t found = 0;
    size_t i;
    int rc;

    if (v == NULL || read == NULL || json_buf == NULL || json_cap < 512u) {
        return LUKS_ERR_BAD_ARG;
    }
    memset(v, 0, sizeof *v);
    v->read = read;
    v->read_ctx = read_ctx;

    if (check_header(v, 0, 0, json_buf, json_cap, &cand[found]) == LUKS_OK) {
        found++;
    }

    if (found == 1u) {
        /* The secondary sits immediately after the primary. */
        if (check_header(v, cand[0].hdr_size, 1, json_buf, json_cap,
                         &cand[found]) == LUKS_OK) {
            found++;
        }
    } else {
        for (i = 0; i < sizeof secondary_offsets / sizeof secondary_offsets[0];
             i++) {
            if (check_header(v, secondary_offsets[i], 1, json_buf, json_cap,
                             &cand[found]) == LUKS_OK) {
                found++;
                break;
            }
        }
    }

    if (found == 0u) {
        return LUKS_ERR_BAD_HEADER;
    }
    best = cand[0];
    for (i = 1; i < found; i++) {
        if (cand[i].seqid > best.seqid) {
            best = cand[i];
        }
    }
    v->used_secondary = (best.off != 0u) ? 1u : 0u;

    rc = load_json(v, &best, json_buf, json_cap);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = root_of(v, &root);
    if (rc != LUKS_OK) {
        return rc;
    }
    return parse_segment(v, &root);
}

/* ---- keyslot enumeration --------------------------------------------- */

static int slot_describe(const json_value *name, const json_value *slot,
                         luks_slot_info *out)
{
    json_value kdf, af;
    uint64_t id = 0;
    uint32_t iters = 0;

    memset(out, 0, sizeof *out);
    out->kdf = LUKS_KDF_UNKNOWN;
    if (json_u64(name, &id) == JSON_OK && id <= 0xffffffffull) {
        out->id = (uint32_t)id;
    }
    if (obj_get(slot, "kdf", &kdf) == LUKS_OK) {
        out->kdf = kdf_of(&kdf);
        if (out->kdf == LUKS_KDF_PBKDF2 &&
            obj_u32(&kdf, "iterations", 0xffffffffu, &iters) == LUKS_OK) {
            out->kdf_iterations = iters;
        }
    }
    (void)obj_u32(slot, "key_size", LUKS_MAX_KEY, &out->key_size);
    if (obj_get(slot, "af", &af) == LUKS_OK) {
        (void)obj_u32(&af, "stripes", LUKS_MAX_STRIPES, &out->af_stripes);
    }
    out->usable = (uint8_t)((out->kdf == LUKS_KDF_PBKDF2 &&
                             obj_str_is(slot, "type", "luks2") &&
                             out->key_size != 0u &&
                             out->af_stripes != 0u) ? 1u : 0u);
    return LUKS_OK;
}

int luks_slot_count(const luks_vol *v, size_t *count)
{
    json_value root, slots;
    json_iter it;
    size_t n = 0;
    int rc;

    if (v == NULL || count == NULL) {
        return LUKS_ERR_BAD_ARG;
    }
    rc = root_of(v, &root);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_get(&root, "keyslots", &slots);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (json_iter_init(&slots, &it) != JSON_OK) {
        return LUKS_ERR_CORRUPT;
    }
    for (;;) {
        json_value k, s;
        int r = json_iter_next(&it, &k, &s);

        if (r < 0) {
            return LUKS_ERR_CORRUPT;
        }
        if (r == 0) {
            break;
        }
        n++;
    }
    *count = n;
    return LUKS_OK;
}

int luks_slot_at(const luks_vol *v, size_t i, luks_slot_info *out)
{
    json_value root, slots;
    json_iter it;
    size_t n = 0;
    int rc;

    if (v == NULL || out == NULL) {
        return LUKS_ERR_BAD_ARG;
    }
    rc = root_of(v, &root);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_get(&root, "keyslots", &slots);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (json_iter_init(&slots, &it) != JSON_OK) {
        return LUKS_ERR_CORRUPT;
    }
    for (;;) {
        json_value k, s;
        int r = json_iter_next(&it, &k, &s);

        if (r < 0) {
            return LUKS_ERR_CORRUPT;
        }
        if (r == 0) {
            return LUKS_ERR_RANGE;
        }
        if (n == i) {
            return slot_describe(&k, &s, out);
        }
        n++;
    }
}

/* ---- unlock ---------------------------------------------------------- */

/* Confirms the recovered key against the digest object that covers this
 * keyslot. Until this passes, "the AF merge produced 64 bytes" says nothing
 * at all: a wrong passphrase produces 64 bytes just as readily. */
static int verify_digest(const json_value *digests, const json_value *name,
                         const luks_master_key *mk)
{
    json_iter it;
    int matched = 0;

    if (json_iter_init(digests, &it) != JSON_OK) {
        return LUKS_ERR_CORRUPT;
    }
    for (;;) {
        json_value dname, dobj, keyslots, segments;
        uint8_t salt[LUKS_MAX_SALT];
        uint8_t want[LUKS_MAX_DIGEST];
        uint8_t got[LUKS_MAX_DIGEST];
        size_t salt_len = 0, want_len = 0;
        const hash_desc *h;
        uint32_t iters;
        int rc;
        int ok;
        int r = json_iter_next(&it, &dname, &dobj);

        if (r < 0) {
            return LUKS_ERR_CORRUPT;
        }
        if (r == 0) {
            break;
        }
        if (obj_get(&dobj, "keyslots", &keyslots) != LUKS_OK) {
            continue;
        }
        if (!arr_has_str(&keyslots, name->ptr, name->len)) {
            continue;
        }
        matched = 1;

        if (!obj_str_is(&dobj, "type", "pbkdf2")) {
            return LUKS_ERR_UNSUPPORTED;
        }
        /* The loader reads segment 0 and nothing else; a digest that does
         * not cover it describes a volume shape we do not handle. */
        if (obj_get(&dobj, "segments", &segments) != LUKS_OK ||
            !arr_has_str(&segments, "0", 1u)) {
            return LUKS_ERR_UNSUPPORTED;
        }
        rc = obj_hash(&dobj, "hash", &h);
        if (rc != LUKS_OK) {
            return rc;
        }
        rc = obj_u32(&dobj, "iterations", 0xffffffffu, &iters);
        if (rc != LUKS_OK) {
            return rc;
        }
        rc = obj_b64(&dobj, "salt", salt, sizeof salt, &salt_len);
        if (rc != LUKS_OK) {
            return rc;
        }
        rc = obj_b64(&dobj, "digest", want, sizeof want, &want_len);
        if (rc != LUKS_OK) {
            return rc;
        }
        if (iters == 0u || want_len == 0u) {
            return LUKS_ERR_CORRUPT;
        }
        /* The master key is the PBKDF2 *password* here, not the salt. */
        if (pbkdf2(h, mk->key, mk->len, salt, salt_len, iters,
                   got, want_len) != CRYPTO_OK) {
            return LUKS_ERR_CORRUPT;
        }
        ok = crypto_ct_eq(got, want, want_len);
        crypto_wipe(got, sizeof got);
        crypto_wipe(salt, sizeof salt);
        if (ok) {
            return LUKS_OK;
        }
        return LUKS_ERR_NOKEY;
    }
    return matched ? LUKS_ERR_NOKEY : LUKS_ERR_CORRUPT;
}

static int try_slot(luks_vol *v, const json_value *name,
                    const json_value *slot, const json_value *digests,
                    const void *pass, size_t pass_len, luks_master_key *mk)
{
    json_value kdf, af, area;
    const hash_desc *kdf_hash, *af_hash;
    uint8_t salt[LUKS_MAX_SALT];
    uint8_t derived[LUKS_MAX_KEY];
    uint8_t stripe[LUKS_MAX_KEY];
    uint8_t sec[LUKS_KEYSLOT_UNIT];
    af_merge_ctx merge;
    xts_ctx xts;
    size_t salt_len = 0;
    uint32_t key_size = 0, stripes = 0, area_key_size = 0, iters = 0;
    uint64_t area_off = 0, area_size = 0;
    uint64_t material, consumed = 0, s;
    uint32_t sfill = 0;
    int rc;

    if (!obj_str_is(slot, "type", "luks2")) {
        return LUKS_ERR_UNSUPPORTED;
    }
    rc = obj_get(slot, "kdf", &kdf);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (kdf_of(&kdf) != LUKS_KDF_PBKDF2) {
        return LUKS_ERR_UNSUPPORTED_KDF;
    }
    rc = obj_u32(slot, "key_size", LUKS_MAX_KEY, &key_size);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_hash(&kdf, "hash", &kdf_hash);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_u32(&kdf, "iterations", 0xffffffffu, &iters);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_b64(&kdf, "salt", salt, sizeof salt, &salt_len);
    if (rc != LUKS_OK) {
        return rc;
    }

    rc = obj_get(slot, "af", &af);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (!obj_str_is(&af, "type", "luks1")) {
        return LUKS_ERR_UNSUPPORTED;
    }
    rc = obj_u32(&af, "stripes", LUKS_MAX_STRIPES, &stripes);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_hash(&af, "hash", &af_hash);
    if (rc != LUKS_OK) {
        return rc;
    }

    rc = obj_get(slot, "area", &area);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (!obj_str_is(&area, "type", "raw") ||
        !obj_str_is(&area, "encryption", "aes-xts-plain64")) {
        return LUKS_ERR_UNSUPPORTED;
    }
    rc = obj_u64(&area, "offset", &area_off);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_u64(&area, "size", &area_size);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_u32(&area, "key_size", LUKS_MAX_KEY, &area_key_size);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (key_size == 0u || stripes == 0u || iters == 0u) {
        return LUKS_ERR_CORRUPT;
    }

    material = (uint64_t)key_size * (uint64_t)stripes;
    if (material > area_size) {
        return LUKS_ERR_CORRUPT;
    }

    /* The area key is the length the AREA declares, not the keyslot's:
     * keyslot.key_size is how much master key is stored, area.key_size is
     * the XTS key that encrypts the stored expansion. They are both 64 for
     * every volume in scope, and conflating them would work by accident
     * until someone made a --key-size 256 volume. */
    rc = pbkdf2(kdf_hash, pass, pass_len, salt, salt_len, iters,
                derived, area_key_size);
    crypto_wipe(salt, sizeof salt);
    if (rc != CRYPTO_OK) {
        return LUKS_ERR_UNSUPPORTED;
    }
    rc = xts_setkey(&xts, derived, area_key_size);
    crypto_wipe(derived, sizeof derived);
    if (rc != CRYPTO_OK) {
        return LUKS_ERR_UNSUPPORTED;
    }

    rc = af_merge_init(&merge, af_hash, key_size, stripes);
    if (rc != AF_OK) {
        xts_wipe(&xts);
        return LUKS_ERR_UNSUPPORTED;
    }

    /* Stream the keyslot area: one 512-byte XTS unit at a time, stripes fed
     * to the merge as they complete. The 250 KB anti-forensic expansion is
     * never resident, so this whole function fits in a small stack frame.
     * Keyslot areas are always 512-based regardless of the data segment's
     * sector size, and the IV counts from 0 at the area's own start. */
    rc = LUKS_OK;
    for (s = 0; consumed < material; s++) {
        uint64_t avail = material - consumed;
        uint32_t take_off = 0;

        if (v->read(v->read_ctx, area_off + s * LUKS_KEYSLOT_UNIT,
                    LUKS_KEYSLOT_UNIT, sec) != 0) {
            rc = LUKS_ERR_IO;
            break;
        }
        xts_decrypt_sector(&xts, s, sec, sec, LUKS_KEYSLOT_UNIT);
        if (avail > (uint64_t)LUKS_KEYSLOT_UNIT) {
            avail = LUKS_KEYSLOT_UNIT;
        }
        while (take_off < (uint32_t)avail) {
            uint32_t take = key_size - sfill;

            if (take > (uint32_t)avail - take_off) {
                take = (uint32_t)avail - take_off;
            }
            memcpy(stripe + sfill, sec + take_off, take);
            sfill += take;
            take_off += take;
            if (sfill == key_size) {
                if (af_merge_stripe(&merge, stripe) != AF_OK) {
                    rc = LUKS_ERR_CORRUPT;
                    break;
                }
                sfill = 0;
            }
        }
        if (rc != LUKS_OK) {
            break;
        }
        consumed += avail;
    }

    xts_wipe(&xts);
    crypto_wipe(sec, sizeof sec);
    crypto_wipe(stripe, sizeof stripe);

    if (rc == LUKS_OK) {
        mk->len = key_size;
        if (af_merge_final(&merge, mk->key) != AF_OK) {
            rc = LUKS_ERR_CORRUPT;
        }
    }
    af_merge_wipe(&merge);
    if (rc != LUKS_OK) {
        crypto_wipe(mk, sizeof *mk);
        return rc;
    }

    rc = verify_digest(digests, name, mk);
    if (rc != LUKS_OK) {
        crypto_wipe(mk, sizeof *mk);
    }
    return rc;
}

int luks_unlock(luks_vol *v, const void *pass, size_t pass_len,
                luks_master_key *mk, uint32_t *slot_id)
{
    json_value root, slots, digests;
    json_iter it;
    int saw_usable = 0;
    int saw_argon2 = 0;
    int rc;

    if (v == NULL || mk == NULL || (pass == NULL && pass_len != 0)) {
        return LUKS_ERR_BAD_ARG;
    }
    memset(mk, 0, sizeof *mk);

    rc = root_of(v, &root);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_get(&root, "keyslots", &slots);
    if (rc != LUKS_OK) {
        return rc;
    }
    rc = obj_get(&root, "digests", &digests);
    if (rc != LUKS_OK) {
        return rc;
    }
    if (json_iter_init(&slots, &it) != JSON_OK) {
        return LUKS_ERR_CORRUPT;
    }

    for (;;) {
        json_value name, slot;
        int r = json_iter_next(&it, &name, &slot);

        if (r < 0) {
            return LUKS_ERR_CORRUPT;
        }
        if (r == 0) {
            break;
        }
        rc = try_slot(v, &name, &slot, &digests, pass, pass_len, mk);
        if (rc == LUKS_OK) {
            if (slot_id != NULL) {
                uint64_t id = 0;

                *slot_id = (json_u64(&name, &id) == JSON_OK &&
                            id <= 0xffffffffull) ? (uint32_t)id : 0u;
            }
            return LUKS_OK;
        }
        if (rc == LUKS_ERR_UNSUPPORTED_KDF) {
            saw_argon2 = 1;
            continue;
        }
        if (rc == LUKS_ERR_IO) {
            return rc;
        }
        if (rc == LUKS_ERR_NOKEY) {
            saw_usable = 1;
            continue;
        }
        /* An unsupported or corrupt slot is not fatal on its own: another
         * slot in the same header may still be one we can open. */
    }

    if (saw_usable) {
        return LUKS_ERR_NOKEY;
    }
    return saw_argon2 ? LUKS_ERR_UNSUPPORTED_KDF : LUKS_ERR_UNSUPPORTED;
}

/* ---- decrypting reader ------------------------------------------------ */

int luks_dev_open(luks_dev *d, luks_vol *v, const luks_master_key *mk)
{
    int rc;

    if (d == NULL || v == NULL || mk == NULL) {
        return LUKS_ERR_BAD_ARG;
    }
    if (v->seg_sector_size == 0u || v->seg_sector_size > LUKS_MAX_SECTOR) {
        return LUKS_ERR_CORRUPT;
    }
    memset(d, 0, sizeof *d);
    rc = xts_setkey(&d->xts, mk->key, mk->len);
    if (rc != CRYPTO_OK) {
        return LUKS_ERR_UNSUPPORTED;
    }
    d->vol = v;
    d->sector_size = v->seg_sector_size;
    d->sec_cached = ~0ull;
    return LUKS_OK;
}

void luks_dev_close(luks_dev *d)
{
    if (d != NULL) {
        xts_wipe(&d->xts);
        crypto_wipe(d->sec_buf, sizeof d->sec_buf);
        d->vol = NULL;
        d->sec_cached = ~0ull;
    }
}

/* plain64 tweak for a plaintext sector.
 *
 * dm-crypt counts the IV in 512-byte sectors even when the encryption unit
 * is larger: it advances cc_sector by sector_size/512 per unit. The
 * iv_large_sectors flag is the opt-out, and it divides back down rather
 * than changing the base. Encoding this here, once, is why xts.h takes a
 * ready-made sector number instead of a byte offset. */
static uint64_t iv_for(const luks_dev *d, uint64_t sec)
{
    uint32_t per = d->sector_size / 512u;
    uint64_t iv = d->vol->seg_iv_tweak + sec * (uint64_t)per;

    if (d->vol->seg_iv_large) {
        uint32_t shift = 0;

        while ((1u << shift) < per) {
            shift++;
        }
        iv >>= shift;
    }
    return iv;
}

int luks_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    luks_dev *d = (luks_dev *)ctx;
    uint8_t *out = (uint8_t *)buf;
    uint64_t ss;

    if (d == NULL || d->vol == NULL || (buf == NULL && len != 0)) {
        return LUKS_ERR_BAD_ARG;
    }
    if (len == 0) {
        return LUKS_OK;
    }
    if (off + len < off) {
        return LUKS_ERR_RANGE;
    }
    if (!d->vol->seg_dynamic && d->vol->seg_size != 0u &&
        off + len > d->vol->seg_size) {
        return LUKS_ERR_RANGE;
    }
    ss = d->sector_size;

    while (len != 0) {
        uint64_t sec = off / ss;
        uint64_t within = off % ss;

        if (within == 0u && len >= ss) {
            /* Aligned bulk run: one device read for the whole thing, then
             * decrypt in place. Matters on the EFI BlockIO path, where each
             * callback is a firmware call. */
            uint64_t nsec = len / ss;
            uint64_t i;

            if (d->vol->read(d->vol->read_ctx,
                             d->vol->seg_offset + sec * ss,
                             nsec * ss, out) != 0) {
                return LUKS_ERR_IO;
            }
            for (i = 0; i < nsec; i++) {
                uint8_t *p = out + i * ss;

                xts_decrypt_sector(&d->xts, iv_for(d, sec + i), p, p,
                                   (size_t)ss);
            }
            out += nsec * ss;
            off += nsec * ss;
            len -= nsec * ss;
        } else {
            uint64_t take = ss - within;

            if (take > len) {
                take = len;
            }
            if (d->sec_cached != sec) {
                if (d->vol->read(d->vol->read_ctx,
                                 d->vol->seg_offset + sec * ss,
                                 ss, d->sec_buf) != 0) {
                    d->sec_cached = ~0ull;
                    return LUKS_ERR_IO;
                }
                xts_decrypt_sector(&d->xts, iv_for(d, sec), d->sec_buf,
                                   d->sec_buf, (size_t)ss);
                d->sec_cached = sec;
            }
            memcpy(out, d->sec_buf + within, (size_t)take);
            out += take;
            off += take;
            len -= take;
        }
    }
    return LUKS_OK;
}
