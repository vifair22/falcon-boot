/* WIM LZX decompression. See falcon-core/src/wim/lzx.h.
 *
 * The bitstream is a sequence of 16-bit little-endian words read most
 * significant bit first; uncompressed blocks drop back to bytes. Codes are
 * canonical Huffman codes of at most 16 bits, decoded here by length (the
 * count-and-first-code method) rather than through a lookup table: a WIM
 * metadata resource is a few megabytes and the loader reads one per boot, so
 * table-free decoding costs nothing that matters and removes the table
 * construction, which is where a decoder handed hostile lengths usually goes
 * wrong.
 *
 * The code lengths themselves are sent as differences from the previous
 * block's, through a 20-symbol "pretree"; within one chunk the main and
 * length trees' lengths carry over from block to block and start at zero.
 */

#include <wim/lzx.h>

#include <lib/string.h>

#define NUM_CHARS        256u
#define NUM_SLOTS        30u      /* position slots for a 32 KiB window */
#define MAIN_SYMS        (NUM_CHARS + 8u * NUM_SLOTS)
#define LEN_SYMS         249u
#define ALIGNED_SYMS     8u
#define PRE_SYMS         20u
#define MAX_CODE_LEN     16u
#define MIN_MATCH        2u
#define NUM_PRIMARY_LENS 7u
#define DEFAULT_BLOCK    32768u
#define E8_FILE_SIZE     12000000

enum { BLOCK_VERBATIM = 1, BLOCK_ALIGNED = 2, BLOCK_UNCOMPRESSED = 3 };

static const uint32_t g_slot_base[NUM_SLOTS] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384,
    512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576
};
static const uint8_t g_slot_bits[NUM_SLOTS] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13
};

/* ---- the bitstream ---- */

typedef struct {
    const uint8_t *p, *end;
    uint32_t bits;      /* next bits, most significant first */
    unsigned left;      /* how many of them are valid */
} bitin;

/* Past the end reads zeros, as wimlib does; a stream that needs them is cut
 * short and fails the output length or a code instead. No caller asks for
 * more than 17 bits, so a refill starts with at most 16 valid and the word
 * always fits; the bound on left says so where the shift can see it. */
static void ensure(bitin *b, unsigned n)
{
    while (b->left < n && b->left <= 16u) {
        uint32_t w = 0;

        if (b->end - b->p >= 2) {
            w = (uint32_t)b->p[0] | ((uint32_t)b->p[1] << 8);
            b->p += 2;
        } else {
            b->p = b->end;
        }
        b->bits |= w << (16u - b->left);
        b->left += 16u;
    }
}

static uint32_t read_bits(bitin *b, unsigned n)
{
    uint32_t v;

    if (n == 0)
        return 0;
    ensure(b, n);
    v = b->bits >> (32u - n);
    b->bits <<= n;
    b->left -= n;
    return v;
}

/* ---- canonical Huffman codes ---- */

typedef struct {
    uint16_t count[MAX_CODE_LEN + 1u];
    uint16_t first[MAX_CODE_LEN + 1u];   /* first code of each length */
    uint16_t index[MAX_CODE_LEN + 1u];   /* where its symbols start */
    uint16_t sym[MAIN_SYMS];             /* symbols by length, then value */
    uint16_t nsyms;
} code;

/* Builds the decoder for lens[0..n). An empty code (every length zero) is
 * legal and decodes nothing; an over-subscribed one is corrupt. An
 * incomplete one is allowed, as LZX encoders emit them for a single symbol,
 * and its unused patterns fail when met. */
static int build(code *c, const uint8_t *lens, unsigned n)
{
    uint32_t code_val = 0, idx = 0;
    unsigned i, l;
    int32_t avail = 1;

    memset(c->count, 0, sizeof c->count);
    for (i = 0; i < n; i++) {
        if (lens[i] > MAX_CODE_LEN)
            return LZX_ERR_CORRUPT;
        c->count[lens[i]]++;
    }
    c->count[0] = 0;
    for (l = 1; l <= MAX_CODE_LEN; l++) {
        avail = avail * 2 - (int32_t)c->count[l];
        if (avail < 0)
            return LZX_ERR_CORRUPT;
        c->first[l] = (uint16_t)code_val;
        c->index[l] = (uint16_t)idx;
        idx += c->count[l];
        code_val = (code_val + c->count[l]) << 1;
    }
    {
        uint16_t fill[MAX_CODE_LEN + 1u];

        memcpy(fill, c->index, sizeof fill);
        for (i = 0; i < n; i++) {
            if (lens[i] != 0)
                c->sym[fill[lens[i]]++] = (uint16_t)i;
        }
    }
    c->nsyms = (uint16_t)idx;
    return LZX_OK;
}

static int decode(const code *c, bitin *b)
{
    uint32_t v = 0;
    unsigned l;

    for (l = 1; l <= MAX_CODE_LEN; l++) {
        uint32_t off;

        v |= read_bits(b, 1);
        off = v - c->first[l];
        if (off < c->count[l])
            return c->sym[c->index[l] + off];
        v <<= 1;
    }
    return -1;
}

/* ---- the decoder ---- */

typedef struct {
    uint8_t main_lens[MAIN_SYMS];
    uint8_t len_lens[LEN_SYMS];
    uint8_t aligned_lens[ALIGNED_SYMS];
    code main, len, aligned, pre;
} lzx_state;

/* One chunk at a time, so one state; a WIM is read from one thread. */
static lzx_state g_s;

/* lens[0..n) as differences from their previous values, through a pretree
 * sent first. */
static int read_lens(bitin *b, uint8_t *lens, unsigned n)
{
    uint8_t pre[PRE_SYMS];
    unsigned i = 0;

    for (i = 0; i < PRE_SYMS; i++)
        pre[i] = (uint8_t)read_bits(b, 4);
    if (build(&g_s.pre, pre, PRE_SYMS) != LZX_OK)
        return LZX_ERR_CORRUPT;
    i = 0;
    while (i < n) {
        int sym = decode(&g_s.pre, b);
        unsigned run;
        int v;

        if (sym < 0)
            return LZX_ERR_CORRUPT;
        if (sym < 17) {
            v = (int)lens[i] - sym;
            lens[i++] = (uint8_t)(v < 0 ? v + 17 : v);
            continue;
        }
        if (sym == 17) {
            run = 4u + read_bits(b, 4);
            v = 0;
        } else if (sym == 18) {
            run = 20u + read_bits(b, 5);
            v = 0;
        } else {
            int s2;

            run = 4u + read_bits(b, 1);
            s2 = decode(&g_s.pre, b);
            if (s2 < 0 || s2 > 16)
                return LZX_ERR_CORRUPT;
            v = (int)lens[i] - s2;
            if (v < 0)
                v += 17;
        }
        if (run > n - i)
            return LZX_ERR_CORRUPT;
        memset(lens + i, v, run);
        i += run;
    }
    return LZX_OK;
}

/* The inverse of the encoder's CALL translation: an absolute target the
 * encoder wrote back to the relative one the code had. */
static void undo_e8(uint8_t *out, size_t n)
{
    size_t i;

    if (n <= 10)
        return;
    for (i = 0; i < n - 10u;) {
        if (out[i] != 0xe8u) {
            i++;
            continue;
        }
        {
            int32_t abs = (int32_t)((uint32_t)out[i + 1] |
                                    ((uint32_t)out[i + 2] << 8) |
                                    ((uint32_t)out[i + 3] << 16) |
                                    ((uint32_t)out[i + 4] << 24));
            int32_t pos = (int32_t)i;
            int32_t rel;
            int change = 0;

            if (abs >= 0) {
                if (abs < E8_FILE_SIZE) {
                    rel = abs - pos;
                    change = 1;
                }
            } else if (abs >= -pos) {
                rel = abs + E8_FILE_SIZE;
                change = 1;
            }
            if (change) {
                uint32_t r = (uint32_t)rel;

                out[i + 1] = (uint8_t)r;
                out[i + 2] = (uint8_t)(r >> 8);
                out[i + 3] = (uint8_t)(r >> 16);
                out[i + 4] = (uint8_t)(r >> 24);
            }
        }
        i += 5u;
    }
}

int lzx_decompress(const uint8_t *in, size_t in_len, uint8_t *out,
                   size_t out_len)
{
    bitin b;
    uint32_t r0 = 1, r1 = 1, r2 = 1;
    size_t pos = 0;

    if (in == (const uint8_t *)0 || out == (uint8_t *)0 || out_len == 0 ||
        out_len > LZX_CHUNK_MAX)
        return LZX_ERR_BAD_ARG;
    memset(g_s.main_lens, 0, sizeof g_s.main_lens);
    memset(g_s.len_lens, 0, sizeof g_s.len_lens);
    b.p = in;
    b.end = in + in_len;
    b.bits = 0;
    b.left = 0;

    while (pos < out_len) {
        unsigned type = read_bits(&b, 3);
        size_t size, end;

        if (read_bits(&b, 1))
            size = DEFAULT_BLOCK;
        else
            size = read_bits(&b, 16);
        if (size == 0 || size > out_len - pos)
            return LZX_ERR_CORRUPT;
        end = pos + size;

        if (type == BLOCK_UNCOMPRESSED) {
            const uint8_t *p;
            unsigned k;

            /* Align to a word; an already aligned stream skips a whole
             * one. Then three offsets and the bytes, padded to a word. */
            ensure(&b, 1);
            b.bits = 0;
            b.left = 0;
            p = b.p;
            if ((size_t)(b.end - p) < 12u + size)
                return LZX_ERR_CORRUPT;
            for (k = 0; k < 3u; k++) {
                uint32_t v = (uint32_t)p[(size_t)4u * k] |
                             ((uint32_t)p[4u * k + 1u] << 8) |
                             ((uint32_t)p[4u * k + 2u] << 16) |
                             ((uint32_t)p[4u * k + 3u] << 24);

                if (k == 0)
                    r0 = v;
                else if (k == 1)
                    r1 = v;
                else
                    r2 = v;
            }
            p += 12;
            memcpy(out + pos, p, size);
            p += size;
            if ((size & 1u) != 0 && p < b.end)
                p++;
            b.p = p;
            pos = end;
            continue;
        }
        if (type != BLOCK_VERBATIM && type != BLOCK_ALIGNED)
            return LZX_ERR_CORRUPT;
        if (type == BLOCK_ALIGNED) {
            unsigned k;

            for (k = 0; k < ALIGNED_SYMS; k++)
                g_s.aligned_lens[k] = (uint8_t)read_bits(&b, 3);
            if (build(&g_s.aligned, g_s.aligned_lens, ALIGNED_SYMS) != LZX_OK)
                return LZX_ERR_CORRUPT;
        }
        if (read_lens(&b, g_s.main_lens, NUM_CHARS) != LZX_OK ||
            read_lens(&b, g_s.main_lens + NUM_CHARS, MAIN_SYMS - NUM_CHARS) !=
                LZX_OK ||
            build(&g_s.main, g_s.main_lens, MAIN_SYMS) != LZX_OK ||
            read_lens(&b, g_s.len_lens, LEN_SYMS) != LZX_OK ||
            build(&g_s.len, g_s.len_lens, LEN_SYMS) != LZX_OK)
            return LZX_ERR_CORRUPT;

        while (pos < end) {
            int sym = decode(&g_s.main, &b);
            unsigned slot, len;
            uint32_t off;

            if (sym < 0)
                return LZX_ERR_CORRUPT;
            if ((unsigned)sym < NUM_CHARS) {
                out[pos++] = (uint8_t)sym;
                continue;
            }
            sym -= (int)NUM_CHARS;
            slot = (unsigned)sym >> 3;
            len = (unsigned)sym & 7u;
            if (len == NUM_PRIMARY_LENS) {
                int ls = decode(&g_s.len, &b);

                if (ls < 0)
                    return LZX_ERR_CORRUPT;
                len += (unsigned)ls;
            }
            len += MIN_MATCH;

            if (slot == 0) {
                off = r0;
            } else if (slot == 1) {
                off = r1;
                r1 = r0;
                r0 = off;
            } else if (slot == 2) {
                off = r2;
                r2 = r0;
                r0 = off;
            } else {
                unsigned nb = g_slot_bits[slot];
                uint32_t extra;

                if (type == BLOCK_ALIGNED && nb >= 3u) {
                    int as;

                    extra = read_bits(&b, nb - 3u) << 3;
                    as = decode(&g_s.aligned, &b);
                    if (as < 0)
                        return LZX_ERR_CORRUPT;
                    extra += (uint32_t)as;
                } else {
                    extra = read_bits(&b, nb);
                }
                off = g_slot_base[slot] + extra - 2u;
                r2 = r1;
                r1 = r0;
                r0 = off;
            }
            if (off == 0 || off > pos || len > end - pos)
                return LZX_ERR_CORRUPT;
            while (len-- > 0) {
                out[pos] = out[pos - off];
                pos++;
            }
        }
    }
    undo_e8(out, out_len);
    return LZX_OK;
}
