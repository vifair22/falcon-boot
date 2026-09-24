/* The EFI-free half of the NXT rows. See nxtcore.h. */

#include <nxt/nxtcore.h>

/* EFI_LOAD_OPTION: UINT32 Attributes, UINT16 FilePathListLength, then the
 * description as a NUL-terminated CHAR16 string, then FilePathListLength
 * bytes of device path, then optional data. Little-endian throughout, and
 * the description is not aligned to anything, so it is read a byte pair at a
 * time rather than through a uint16_t pointer. */
#define OPT_HEADER 6u

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int nxt_option_parse(const uint8_t *buf, size_t len, uint32_t *attrs,
                     uint16_t label[NXT_LABEL_MAX])
{
    size_t off = OPT_HEADER;
    size_t n = 0;
    uint16_t path_len;

    label[0] = 0;
    if (buf == (const uint8_t *)0 || len < OPT_HEADER + 2u)
        return NXT_ERR_SHORT;
    *attrs = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
             ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    path_len = get16(buf + 4);

    for (;;) {
        uint16_t c;

        if (off + 2u > len) {
            label[0] = 0;
            return NXT_ERR_DESC;
        }
        c = get16(buf + off);
        off += 2u;
        if (c == 0)
            break;
        if (n + 1u < NXT_LABEL_MAX)
            label[n++] = c;
    }
    label[n] = 0;

    /* The path is not read, only required to fit: an entry whose lengths
     * disagree with its size was written by something that got the format
     * wrong, and what it names is anybody's guess. */
    if ((size_t)path_len > len - off) {
        label[0] = 0;
        return NXT_ERR_PATH;
    }
    return NXT_OK;
}

int nxt_option_offered(uint32_t attrs)
{
    if ((attrs & NXT_LOAD_OPTION_ACTIVE) == 0)
        return 0;
    if ((attrs & NXT_LOAD_OPTION_HIDDEN) != 0)
        return 0;
    return (attrs & NXT_LOAD_OPTION_CATEGORY) == NXT_LOAD_OPTION_CATEGORY_BOOT;
}

size_t nxt_order_parse(const uint8_t *buf, size_t len, uint16_t *out,
                       size_t cap)
{
    size_t n = 0;
    size_t i;

    if (buf == (const uint8_t *)0 || out == (uint16_t *)0)
        return 0;
    for (i = 0; i + 2u <= len && n < cap; i += 2u) {
        uint16_t v = get16(buf + i);
        size_t k;
        int seen = 0;

        for (k = 0; k < n; k++) {
            if (out[k] == v) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            out[n++] = v;
    }
    return n;
}
