/* falcon-core: a gzip member, inflated. See gz.h.
 *
 * Written from RFC 1951 and RFC 1952. The Huffman decoding is the canonical
 * one the RFC describes: a code is read a bit at a time and compared against
 * the first code of each length, which needs only the count of codes per
 * length and the symbols in code order, not a lookup table. It is slower
 * than a table and a good deal smaller, and stage 2's whole job is to be
 * small; a floppy's worth of output inflates in milliseconds either way.
 */

#include <gz/gz.h>

#define MAXBITS  15u
#define NLIT     288u
#define NDIST    30u
#define NCLEN    19u

typedef struct {
    const uint8_t *in;
    size_t n;
    size_t pos;
    uint32_t bits;
    unsigned nbits;
    uint8_t *out;
    size_t cap;
    size_t len;
} state;

typedef struct {
    uint16_t count[MAXBITS + 1u];
    uint16_t symbol[NLIT];
} huff;

/* `need` bits, least significant first; -1 past the end of the input. */
static int getbits(state *s, unsigned need, uint32_t *v)
{
    while (s->nbits < need) {
        if (s->pos >= s->n)
            return -1;
        s->bits |= (uint32_t)s->in[s->pos++] << s->nbits;
        s->nbits += 8u;
    }
    *v = s->bits & ((1u << need) - 1u);
    s->bits >>= need;
    s->nbits -= need;
    return 0;
}

/* Builds `h` from `n` code lengths. 0 on a complete or incomplete code,
 * -1 on an over-subscribed one, which no valid stream carries. */
static int build(huff *h, const uint8_t *lengths, unsigned n)
{
    uint16_t offs[MAXBITS + 1u];
    int left = 1;
    unsigned len;
    unsigned sym;

    for (len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (sym = 0; sym < n; sym++)
        h->count[lengths[sym]]++;
    if (h->count[0] == n)
        return 0;                    /* no codes: complete, and unusable */
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1u] = (uint16_t)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++) {
        if (lengths[sym] != 0)
            h->symbol[offs[lengths[sym]]++] = (uint16_t)sym;
    }
    return 0;
}

/* One symbol through `h`; -1 on running out of input or a code `h` does
 * not have. */
static int decode(state *s, const huff *h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    unsigned len;

    for (len = 1; len <= MAXBITS; len++) {
        uint32_t b;
        int count;

        if (getbits(s, 1, &b) != 0)
            return -1;
        code |= (int)b;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* The literal/length and distance codes of one block, until its end. */
static gz_status codes(state *s, const huff *lit, const huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        uint32_t extra;
        unsigned length;
        unsigned d;

        if (sym < 0)
            return GZ_ERR_DATA;
        if (sym < 256) {
            if (s->len >= s->cap)
                return GZ_ERR_SPACE;
            s->out[s->len++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256)
            return GZ_OK;
        sym -= 257;
        if (sym >= 29)
            return GZ_ERR_DATA;
        if (getbits(s, k_len_extra[sym], &extra) != 0)
            return GZ_ERR_DATA;
        length = k_len_base[sym] + extra;
        sym = decode(s, dist);
        if (sym < 0 || sym >= 30)
            return GZ_ERR_DATA;
        if (getbits(s, k_dist_extra[sym], &extra) != 0)
            return GZ_ERR_DATA;
        d = k_dist_base[sym] + extra;
        if (d > s->len)
            return GZ_ERR_DATA;      /* reaches before the start */
        if (length > s->cap - s->len)
            return GZ_ERR_SPACE;
        /* Byte by byte: a distance shorter than the length repeats what it
         * has just written, which is how deflate says "run". */
        while (length-- > 0) {
            s->out[s->len] = s->out[s->len - d];
            s->len++;
        }
    }
}

static gz_status stored(state *s)
{
    uint16_t len;
    uint16_t nlen;

    /* To the next byte boundary, then LEN and its complement. */
    s->bits = 0;
    s->nbits = 0;
    if (s->pos + 4u > s->n)
        return GZ_ERR_DATA;
    len = (uint16_t)((unsigned)s->in[s->pos] | ((unsigned)s->in[s->pos + 1u] << 8));
    nlen = (uint16_t)((unsigned)s->in[s->pos + 2u] |
                      ((unsigned)s->in[s->pos + 3u] << 8));
    s->pos += 4u;
    if ((uint16_t)~nlen != len)
        return GZ_ERR_DATA;
    if (len > s->n - s->pos)
        return GZ_ERR_DATA;
    if (len > s->cap - s->len)
        return GZ_ERR_SPACE;
    while (len-- > 0)
        s->out[s->len++] = s->in[s->pos++];
    return GZ_OK;
}

static gz_status fixed(state *s)
{
    huff lit;
    huff dist;
    uint8_t lengths[NLIT];
    unsigned i;

    for (i = 0; i < 144u; i++)
        lengths[i] = 8;
    for (; i < 256u; i++)
        lengths[i] = 9;
    for (; i < 280u; i++)
        lengths[i] = 7;
    for (; i < NLIT; i++)
        lengths[i] = 8;
    (void)build(&lit, lengths, NLIT);
    for (i = 0; i < NDIST; i++)
        lengths[i] = 5;
    (void)build(&dist, lengths, NDIST);
    return codes(s, &lit, &dist);
}

static gz_status dynamic(state *s)
{
    static const uint8_t order[NCLEN] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    huff lit;
    huff dist;
    uint8_t lengths[NLIT + NDIST];
    uint32_t v;
    unsigned nlen;
    unsigned ndist;
    unsigned ncode;
    unsigned i;

    if (getbits(s, 5, &v) != 0)
        return GZ_ERR_DATA;
    nlen = v + 257u;
    if (getbits(s, 5, &v) != 0)
        return GZ_ERR_DATA;
    ndist = v + 1u;
    if (getbits(s, 4, &v) != 0)
        return GZ_ERR_DATA;
    ncode = v + 4u;
    if (nlen > 286u || ndist > NDIST)
        return GZ_ERR_DATA;

    for (i = 0; i < NCLEN; i++)
        lengths[order[i]] = 0;
    for (i = 0; i < ncode; i++) {
        if (getbits(s, 3, &v) != 0)
            return GZ_ERR_DATA;
        lengths[order[i]] = (uint8_t)v;
    }
    if (build(&lit, lengths, NCLEN) != 0)
        return GZ_ERR_DATA;

    for (i = 0; i < nlen + ndist;) {
        int sym = decode(s, &lit);
        unsigned rep;
        uint8_t val = 0;

        if (sym < 0)
            return GZ_ERR_DATA;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (i == 0)
                return GZ_ERR_DATA;  /* nothing to repeat */
            val = lengths[i - 1u];
            if (getbits(s, 2, &v) != 0)
                return GZ_ERR_DATA;
            rep = 3u + v;
        } else if (sym == 17) {
            if (getbits(s, 3, &v) != 0)
                return GZ_ERR_DATA;
            rep = 3u + v;
        } else {
            if (getbits(s, 7, &v) != 0)
                return GZ_ERR_DATA;
            rep = 11u + v;
        }
        if (i + rep > nlen + ndist)
            return GZ_ERR_DATA;
        while (rep-- > 0)
            lengths[i++] = val;
    }
    if (lengths[256] == 0)
        return GZ_ERR_DATA;          /* no end-of-block code */
    if (build(&lit, lengths, nlen) != 0 ||
        build(&dist, lengths + nlen, ndist) != 0)
        return GZ_ERR_DATA;
    return codes(s, &lit, &dist);
}

gz_status gz_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                     size_t *out_len, size_t *used)
{
    state s;
    uint32_t last;
    uint32_t type;
    gz_status rc;

    if (in == (const uint8_t *)0 || out == (uint8_t *)0 ||
        out_len == (size_t *)0)
        return GZ_ERR_ARG;
    s.in = in;
    s.n = n;
    s.pos = 0;
    s.bits = 0;
    s.nbits = 0;
    s.out = out;
    s.cap = cap;
    s.len = 0;
    do {
        if (getbits(&s, 1, &last) != 0 || getbits(&s, 2, &type) != 0)
            return GZ_ERR_DATA;
        if (type == 0)
            rc = stored(&s);
        else if (type == 1)
            rc = fixed(&s);
        else if (type == 2)
            rc = dynamic(&s);
        else
            rc = GZ_ERR_DATA;
        if (rc != GZ_OK)
            return rc;
    } while (!last);
    *out_len = s.len;
    if (used != (size_t *)0)
        *used = s.pos;               /* the partial last byte is spent */
    return GZ_OK;
}

uint32_t gz_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n-- > 0) {
        unsigned k;

        crc ^= *p++;
        for (k = 0; k < 8u; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

gz_status gz_member(const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                    size_t *out_len)
{
    size_t pos = 10;
    size_t used;
    uint8_t flags;
    gz_status rc;

    if (in == (const uint8_t *)0 || out == (uint8_t *)0 ||
        out_len == (size_t *)0)
        return GZ_ERR_ARG;
    if (n < 18u || in[0] != 0x1Fu || in[1] != 0x8Bu || in[2] != 8u)
        return GZ_ERR_FORMAT;
    flags = in[3];
    if ((flags & 0xE0u) != 0)
        return GZ_ERR_FORMAT;        /* reserved flags: not ours to guess */
    if ((flags & 0x04u) != 0) {      /* FEXTRA */
        size_t xlen;

        if (pos + 2u > n)
            return GZ_ERR_FORMAT;
        xlen = (size_t)in[pos] | ((size_t)in[pos + 1u] << 8);
        pos += 2u + xlen;
    }
    if ((flags & 0x08u) != 0) {      /* FNAME */
        while (pos < n && in[pos] != 0)
            pos++;
        pos++;
    }
    if ((flags & 0x10u) != 0) {      /* FCOMMENT */
        while (pos < n && in[pos] != 0)
            pos++;
        pos++;
    }
    if ((flags & 0x02u) != 0)        /* FHCRC */
        pos += 2u;
    if (pos + 8u > n)
        return GZ_ERR_FORMAT;

    rc = gz_inflate(in + pos, n - pos - 8u, out, cap, out_len, &used);
    if (rc != GZ_OK)
        return rc;
    pos += used;
    if (pos + 8u > n)
        return GZ_ERR_DATA;
    if (rd32(in + pos) != gz_crc32(0, out, *out_len) ||
        rd32(in + pos + 4u) != (uint32_t)*out_len)
        return GZ_ERR_CHECK;
    return GZ_OK;
}
