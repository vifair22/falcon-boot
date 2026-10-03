/* falcon-boot: Argon2id (RFC 9106). See argon2.h.
 *
 * Written against RFC 9106 Section 3 and checked, bit for bit, against the
 * reference implementation's output (the argon2 CLI and the RFC's own
 * Section 5.3 vector; falcon-core/tests/argon2_test.c). Where the RFC's prose
 * and the reference differ in form, the reference's form is kept, because it
 * is what every existing keyslot was derived with: the index arithmetic in
 * ref_index mirrors index_alpha in the reference's core.c, down to which
 * quantities are 32-bit and which 64-bit.
 *
 * Memory layout: lane l's column j is block l * q + j, the reference's
 * layout, so "the previous block" and "the lane's last block" are the same
 * offsets the reference computes.
 */

#include <crypto/argon2.h>
#include <crypto/argon2_simd.h>
#include <crypto/blake2b.h>
#include <crypto/crypto.h>
#include <lib/string.h>

#define QWORDS        (ARGON2_BLOCK_BYTES / 8u)   /* 128 */
#define SYNC_POINTS   4u                          /* slices per pass */
#define ADDR_PER_BLK  QWORDS                      /* addresses per block */
#define TYPE_ID       2u                          /* y for Argon2id */
#define H0_LEN        64u
#define PRIME_IN_LEN  (H0_LEN + 8u)               /* H0 || LE32 j || LE32 i */

typedef struct {
    uint64_t v[QWORDS];
} blk;

/* The scratch blocks one derivation needs, kept together so they are wiped
 * once at the end rather than once per compression. */
typedef struct {
    blk r;            /* G's working copy */
    blk input;        /* data-independent addressing: the counter block */
    blk address;      /* ... and the 128 pseudo-random values it yields */
    uint64_t simd[ARGON2_SIMD_SCRATCH_WORDS];   /* a vector fill's state */
} work;

typedef struct {
    blk     *mem;
    uint32_t lanes;
    uint32_t q;       /* lane length in blocks */
    uint32_t sl;      /* segment length in blocks, q / 4 */
    uint32_t mprime;  /* total blocks */
    uint32_t passes;
    argon2_fill_fn fill;   /* a vector level's block function, or NULL */
    void (*wipe)(void);    /* ... and its register wipe */
} inst;

static uint64_t ror64(uint64_t x, unsigned int n)
{
    return (x >> n) | (x << (64u - n));
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* ---- H' (RFC 9106 Section 3.3) ---------------------------------------- */

/* H'^T(in): BLAKE2b at the requested length when it fits in one output,
 * otherwise a chain of 64-byte outputs of which the first 32 bytes each are
 * kept, closed by one output of whatever remains. */
static void h_prime(uint8_t *out, uint32_t outlen, const uint8_t *in,
                    size_t inlen)
{
    blake2b_ctx c;
    uint8_t le[4];
    uint8_t v[BLAKE2B_OUT_MAX];
    uint32_t left;

    put_le32(le, outlen);
    if (outlen <= BLAKE2B_OUT_MAX) {
        (void)blake2b_init(&c, outlen, NULL, 0);
        blake2b_update(&c, le, sizeof le);
        blake2b_update(&c, in, inlen);
        blake2b_final(&c, out);
        return;
    }
    (void)blake2b_init(&c, BLAKE2B_OUT_MAX, NULL, 0);
    blake2b_update(&c, le, sizeof le);
    blake2b_update(&c, in, inlen);
    blake2b_final(&c, v);
    memcpy(out, v, 32u);
    out += 32;
    left = outlen - 32u;
    while (left > BLAKE2B_OUT_MAX) {
        (void)blake2b(v, BLAKE2B_OUT_MAX, NULL, 0, v, sizeof v);
        memcpy(out, v, 32u);
        out += 32;
        left -= 32u;
    }
    (void)blake2b(out, left, NULL, 0, v, sizeof v);
    crypto_wipe(v, sizeof v);
}

/* ---- G, the compression function (RFC 9106 Section 3.5, 3.6) ---------- */

/* BLAKE2b's G with the addition replaced by the multiply-hardened
 * x + y + 2 * lo32(x) * lo32(y). */
static uint64_t fbla(uint64_t x, uint64_t y)
{
    const uint64_t m = 0xffffffffull;

    return x + y + 2u * ((x & m) * (y & m));
}

#define GB(a, b, c, d)                   \
    do {                                 \
        (a) = fbla((a), (b));            \
        (d) = ror64((d) ^ (a), 32u);     \
        (c) = fbla((c), (d));            \
        (b) = ror64((b) ^ (c), 24u);     \
        (a) = fbla((a), (b));            \
        (d) = ror64((d) ^ (a), 16u);     \
        (c) = fbla((c), (d));            \
        (b) = ror64((b) ^ (c), 63u);     \
    } while (0)

/* P over sixteen words of r, named by idx: BLAKE2b's round without the
 * message, four columns then four diagonals. */
static void perm(uint64_t *r, const uint8_t *idx)
{
    uint64_t v[16];
    unsigned int i;

    for (i = 0; i < 16u; i++) {
        v[i] = r[idx[i]];
    }
    GB(v[0], v[4], v[8],  v[12]);
    GB(v[1], v[5], v[9],  v[13]);
    GB(v[2], v[6], v[10], v[14]);
    GB(v[3], v[7], v[11], v[15]);
    GB(v[0], v[5], v[10], v[15]);
    GB(v[1], v[6], v[11], v[12]);
    GB(v[2], v[7], v[8],  v[13]);
    GB(v[3], v[4], v[9],  v[14]);
    for (i = 0; i < 16u; i++) {
        r[idx[i]] = v[i];
    }
}

/* next = G(prev, ref), or next ^= G(prev, ref) from the second pass on
 * (version 0x13's overwrite rule). prev NULL stands for the zero block,
 * which is all the address generator ever passes.
 *
 * The reference keeps the pre-permutation R in a second temporary; here next
 * holds it instead, which is the same sum with one block less of stack. R is
 * complete before next is written, so ref may be next (the address block is
 * compressed in place). */
static void fill_block(blk *r, const blk *prev, const blk *ref, blk *next,
                       int with_xor)
{
    uint8_t idx[16];
    unsigned int i, k;

    for (i = 0; i < QWORDS; i++) {
        r->v[i] = ref->v[i] ^ ((prev != NULL) ? prev->v[i] : 0u);
    }
    for (i = 0; i < QWORDS; i++) {
        next->v[i] = with_xor ? (next->v[i] ^ r->v[i]) : r->v[i];
    }
    /* Rows: the eight 16-word runs. */
    for (i = 0; i < 8u; i++) {
        for (k = 0; k < 16u; k++) {
            idx[k] = (uint8_t)(16u * i + k);
        }
        perm(r->v, idx);
    }
    /* Columns: word pairs 2i, 2i+1 of every row. */
    for (i = 0; i < 8u; i++) {
        for (k = 0; k < 8u; k++) {
            idx[(size_t)2u * k] = (uint8_t)(2u * i + 16u * k);
            idx[2u * k + 1u] = (uint8_t)(2u * i + 16u * k + 1u);
        }
        perm(r->v, idx);
    }
    for (i = 0; i < QWORDS; i++) {
        next->v[i] ^= r->v[i];
    }
}

/* The next 128 pseudo-random values of data-independent addressing:
 * address = G(0, G(0, input)) with the counter in input bumped first. */
static void next_addresses(work *w)
{
    w->input.v[6]++;
    fill_block(&w->r, NULL, &w->input, &w->address, 0);
    fill_block(&w->r, NULL, &w->address, &w->address, 0);
}

/* ---- indexing (RFC 9106 Section 3.4.1.1 and 3.4.2) -------------------- */

/* The column in the reference lane that block `index` of this segment
 * compresses against. The window is every finished block the rules allow
 * (the current lane may also use its own segment so far, less the block
 * just before; another lane only its finished slices, and not its last
 * block when this is a segment's first); J1 maps into it non-uniformly,
 * favouring recent blocks. */
static uint32_t ref_index(const inst *in, uint32_t pass, uint32_t slice,
                          uint32_t index, uint32_t j1, int same_lane)
{
    uint32_t area;
    uint64_t rel;
    uint64_t start = 0;

    if (pass == 0u) {
        if (slice == 0u) {
            area = index - 1u;
        } else if (same_lane) {
            area = slice * in->sl + index - 1u;
        } else {
            area = slice * in->sl - ((index == 0u) ? 1u : 0u);
        }
    } else {
        if (same_lane) {
            area = in->q - in->sl + index - 1u;
        } else {
            area = in->q - in->sl - ((index == 0u) ? 1u : 0u);
        }
        start = (slice == SYNC_POINTS - 1u) ? 0u
                                             : (uint64_t)(slice + 1u) * in->sl;
    }
    rel = (uint64_t)j1;
    rel = (rel * rel) >> 32;
    rel = (uint64_t)area - 1u - (((uint64_t)area * rel) >> 32);
    return (uint32_t)((start + rel) % in->q);
}

static void fill_segment(const inst *in, work *w, uint32_t pass,
                         uint32_t slice, uint32_t lane)
{
    blk *mem = in->mem;
    int indep = (pass == 0u && slice < SYNC_POINTS / 2u);
    uint32_t start = 0;
    uint32_t curr, prev, i;

    if (indep) {
        /* The address block is always made before it is read (i == 0, or
         * the start == 2 call below); cleared here so no path can see an
         * earlier segment's addresses. */
        memset(&w->address, 0, sizeof w->address);
        memset(&w->input, 0, sizeof w->input);
        w->input.v[0] = pass;
        w->input.v[1] = lane;
        w->input.v[2] = slice;
        w->input.v[3] = in->mprime;
        w->input.v[4] = in->passes;
        w->input.v[5] = TYPE_ID;
    }
    if (pass == 0u && slice == 0u) {
        /* Columns 0 and 1 came from H'. */
        start = 2;
        if (indep) {
            next_addresses(w);
        }
    }
    curr = lane * in->q + slice * in->sl + start;
    prev = (curr % in->q == 0u) ? curr + in->q - 1u : curr - 1u;

    for (i = start; i < in->sl; i++, curr++, prev++) {
        uint64_t pr;
        uint32_t ref_lane, ref_col;

        if (curr % in->q == 1u) {
            prev = curr - 1u;
        }
        if (indep) {
            if (i % ADDR_PER_BLK == 0u) {
                next_addresses(w);
            }
            pr = w->address.v[i % ADDR_PER_BLK];
        } else {
            pr = mem[prev].v[0];
        }
        ref_lane = (uint32_t)((pr >> 32) % in->lanes);
        if (pass == 0u && slice == 0u) {
            ref_lane = lane;
        }
        ref_col = ref_index(in, pass, slice, i, (uint32_t)pr,
                            ref_lane == lane);
        if (in->fill != NULL) {
            in->fill(w->simd, mem[prev].v,
                     mem[(size_t)in->q * ref_lane + ref_col].v, mem[curr].v,
                     pass != 0u);
        } else {
            fill_block(&w->r, &mem[prev],
                       &mem[(size_t)in->q * ref_lane + ref_col], &mem[curr],
                       pass != 0u);
        }
    }
}

/* ---- one slice's lanes ------------------------------------------------ */

/* What a segment's job needs besides its lane. */
typedef struct {
    const inst *in;
    uint32_t    pass;
    uint32_t    slice;
} slice_ctx;

/* The scratch is the job's own, on whichever stack runs it, so lanes on
 * different processors share nothing but the memory block, and each wipes
 * its own before it returns: the scratch, and on a vector level the
 * registers of the processor that ran it. */
static void segment_job(void *ctx, uint32_t lane)
{
    const slice_ctx *sc = (const slice_ctx *)ctx;
    work w;

    fill_segment(sc->in, &w, sc->pass, sc->slice, lane);
    if (sc->in->wipe != NULL) {
        sc->in->wipe();
    }
    crypto_wipe(&w, sizeof w);
}

static void run_in_turn(void *self, argon2_job job, void *ctx, uint32_t lanes)
{
    uint32_t l;

    (void)self;
    for (l = 0; l < lanes; l++) {
        job(ctx, l);
    }
}

static const argon2_runner in_turn = { run_in_turn, NULL };

/* ---- the whole derivation (RFC 9106 Section 3.2) ---------------------- */

static void blk_from_bytes(blk *b)
{
    const uint8_t *p = (const uint8_t *)b;
    unsigned int i;

    for (i = 0; i < QWORDS; i++) {
        uint64_t x = get_le64(p + (size_t)8u * i);

        b->v[i] = x;
    }
}

static void blk_to_bytes(blk *b)
{
    uint8_t *p = (uint8_t *)b;
    unsigned int i;

    for (i = 0; i < QWORDS; i++) {
        uint64_t x = b->v[i];

        put_le64(p + (size_t)8u * i, x);
    }
}

static void absorb_len(blake2b_ctx *c, uint32_t v)
{
    uint8_t le[4];

    put_le32(le, v);
    blake2b_update(c, le, sizeof le);
}

static void absorb_field(blake2b_ctx *c, const void *p, size_t len)
{
    absorb_len(c, (uint32_t)len);
    if (len != 0u) {
        blake2b_update(c, p, len);
    }
}

/* Past what an RFC 9106 length field holds. Two shifts, because a 32-bit
 * size_t cannot be shifted by 32 and cannot exceed the limit either. */
static int over32(size_t n)
{
    return ((n >> 16) >> 16) != 0u;
}

uint32_t argon2_mem_kib(uint32_t m_cost, uint32_t lanes)
{
    if (lanes == 0u || lanes > ARGON2_MAX_LANES ||
        (uint64_t)m_cost < 8ull * lanes) {
        return 0;
    }
    return (m_cost / (4u * lanes)) * (4u * lanes);
}

/* The block function a level names, or CRYPTO_ERR_UNSUPPORTED when this
 * processor cannot run it. */
static int pick_level(inst *in, int simd)
{
    int best = crypto_simd_level();

    if (simd == ARGON2_SIMD_AUTO) {
        simd = best;
    } else if (simd < CRYPTO_SIMD_NONE || simd > CRYPTO_SIMD_AVX2) {
        return CRYPTO_ERR_BAD_ARG;
    } else if (simd > best) {
        return CRYPTO_ERR_UNSUPPORTED;
    }
    switch (simd) {
    case CRYPTO_SIMD_SSE2:
        in->fill = argon2_fill_sse2;
        in->wipe = argon2_wipe_sse2;
        break;
    case CRYPTO_SIMD_SSSE3:
        in->fill = argon2_fill_ssse3;
        in->wipe = argon2_wipe_ssse3;
        break;
    case CRYPTO_SIMD_AVX2:
        in->fill = argon2_fill_avx2;
        in->wipe = argon2_wipe_avx2;
        break;
    default:
        in->fill = NULL;
        in->wipe = NULL;
        break;
    }
    return CRYPTO_OK;
}

int argon2id(const argon2_params *p, void *mem, uint32_t mem_kib,
             const argon2_runner *run, uint8_t *out, size_t out_len)
{
    return argon2id_simd(p, mem, mem_kib, run, ARGON2_SIMD_AUTO, out,
                         out_len);
}

int argon2id_simd(const argon2_params *p, void *mem, uint32_t mem_kib,
                  const argon2_runner *run, int simd, uint8_t *out,
                  size_t out_len)
{
    int rc;

    blake2b_ctx c;
    uint8_t pin[PRIME_IN_LEN];
    blk tag;
    slice_ctx sc;
    inst in;
    uint32_t l, pass, slice, i;

    if (p == NULL || mem == NULL || out == NULL ||
        (p->pwd == NULL && p->pwd_len != 0u) ||
        p->salt == NULL || p->salt_len < ARGON2_MIN_SALT ||
        (p->secret == NULL && p->secret_len != 0u) ||
        (p->ad == NULL && p->ad_len != 0u) ||
        over32(p->pwd_len) ||
        over32(p->salt_len) ||
        over32(p->secret_len) ||
        over32(p->ad_len) ||
        out_len < ARGON2_MIN_OUT || over32(out_len) ||
        p->t_cost == 0u) {
        return CRYPTO_ERR_BAD_ARG;
    }
    in.mprime = argon2_mem_kib(p->m_cost, p->lanes);
    if (in.mprime == 0u) {
        return CRYPTO_ERR_BAD_ARG;
    }
    if ((uintptr_t)mem % 8u != 0u) {
        return CRYPTO_ERR_BAD_ARG;
    }
    if ((uint64_t)in.mprime * ARGON2_BLOCK_BYTES > (uint64_t)SIZE_MAX) {
        return CRYPTO_ERR_UNSUPPORTED;
    }
    if (mem_kib < in.mprime) {
        return CRYPTO_ERR_NOSPACE;
    }
    if (run == NULL) {
        run = &in_turn;
    } else if (run->run == NULL) {
        return CRYPTO_ERR_BAD_ARG;
    }
    rc = pick_level(&in, simd);
    if (rc != CRYPTO_OK) {
        return rc;
    }
    in.mem = mem;
    in.lanes = p->lanes;
    in.q = in.mprime / p->lanes;
    in.sl = in.q / SYNC_POINTS;
    in.passes = p->t_cost;

    /* H0: every input and parameter, length-prefixed. m is m_cost as given,
     * not m', which is what the reference hashes. */
    (void)blake2b_init(&c, H0_LEN, NULL, 0);
    absorb_len(&c, p->lanes);
    absorb_len(&c, (uint32_t)out_len);
    absorb_len(&c, p->m_cost);
    absorb_len(&c, p->t_cost);
    absorb_len(&c, ARGON2_VERSION);
    absorb_len(&c, TYPE_ID);
    absorb_field(&c, p->pwd, p->pwd_len);
    absorb_field(&c, p->salt, p->salt_len);
    absorb_field(&c, p->secret, p->secret_len);
    absorb_field(&c, p->ad, p->ad_len);
    blake2b_final(&c, pin);

    /* Each lane's first two columns: H'(H0 || LE32(column) || LE32(lane)). */
    for (l = 0; l < in.lanes; l++) {
        for (i = 0; i < 2u; i++) {
            blk *b = &in.mem[(size_t)l * in.q + i];

            put_le32(pin + H0_LEN, i);
            put_le32(pin + H0_LEN + 4u, l);
            h_prime((uint8_t *)b, ARGON2_BLOCK_BYTES, pin, sizeof pin);
            blk_from_bytes(b);
        }
    }
    crypto_wipe(pin, sizeof pin);

    /* Slice by slice, every lane's segment before the next slice: the
     * synchronisation points are the runner's returns. */
    sc.in = &in;
    for (pass = 0; pass < in.passes; pass++) {
        for (slice = 0; slice < SYNC_POINTS; slice++) {
            sc.pass = pass;
            sc.slice = slice;
            run->run(run->self, segment_job, &sc, in.lanes);
        }
    }

    /* The tag: H' over the XOR of every lane's last column. */
    memcpy(&tag, &in.mem[in.q - 1u], sizeof tag);
    for (l = 1; l < in.lanes; l++) {
        const blk *b = &in.mem[(size_t)l * in.q + in.q - 1u];

        for (i = 0; i < QWORDS; i++) {
            tag.v[i] ^= b->v[i];
        }
    }
    blk_to_bytes(&tag);
    h_prime(out, (uint32_t)out_len, (const uint8_t *)&tag,
            ARGON2_BLOCK_BYTES);

    crypto_wipe(&tag, sizeof tag);
    for (i = 0; i < in.mprime; i++) {
        crypto_wipe(&in.mem[i], sizeof in.mem[i]);
    }
    return CRYPTO_OK;
}
