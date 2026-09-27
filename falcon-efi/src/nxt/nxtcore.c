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

/* Device path nodes (UEFI 2.10 Section 10.3): a type, a subtype and a
 * two-byte length, then the node's data. */
#define DP_HDR          4u
#define DP_MEDIA        0x04u
#define DP_MEDIA_HD     0x01u
#define DP_MEDIA_FILE   0x04u
#define DP_END          0x7Fu
#define DP_HD_LEN       42u

int nxt_option_target(const uint8_t *buf, size_t len, nxt_target *out)
{
    size_t off = OPT_HEADER;
    size_t end;
    size_t n = 0;
    int file_ok = 1;
    uint16_t path_len;
    size_t i;

    if (out == (nxt_target *)0)
        return NXT_ERR_SHORT;
    for (i = 0; i < sizeof *out; i++)
        ((uint8_t *)out)[i] = 0;
    if (buf == (const uint8_t *)0 || len < OPT_HEADER + 2u)
        return NXT_ERR_SHORT;
    path_len = get16(buf + 4);
    /* Past the description, which nxt_option_parse has already checked. */
    for (;;) {
        if (off + 2u > len)
            return NXT_ERR_DESC;
        off += 2u;
        if (get16(buf + off - 2u) == 0)
            break;
    }
    if ((size_t)path_len > len - off)
        return NXT_ERR_PATH;
    end = off + path_len;

    while (off + DP_HDR <= end) {
        uint8_t type = buf[off];
        uint8_t sub = buf[off + 1u];
        uint16_t nlen = get16(buf + off + 2u);

        if (nlen < DP_HDR || nlen > end - off)
            return NXT_ERR_PATH;
        if (type == DP_END)
            break;
        if (type == DP_MEDIA && sub == DP_MEDIA_HD && nlen >= DP_HD_LEN) {
            const uint8_t *p = buf + off;

            out->has_hd = 1;
            out->part = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                        ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            out->start = 0;
            for (i = 0; i < 8u; i++)
                out->start |= (uint64_t)p[8u + i] << (8u * i);
            for (i = 0; i < 16u; i++)
                out->sig[i] = p[24u + i];
            out->sig_type = p[41];
        } else if (type == DP_MEDIA && sub == DP_MEDIA_FILE) {
            /* The path's text, NUL-terminated inside the node. A path in
             * several nodes (a directory node, then a file node) is their
             * concatenation, with the separator the firmware would add. */
            size_t k;

            if (n != 0 && out->file[n - 1u] != '\\' &&
                nlen >= DP_HDR + 2u && buf[off + DP_HDR] != '\\' &&
                n + 1u < NXT_PATH_MAX)
                out->file[n++] = '\\';
            for (k = off + DP_HDR; k + 2u <= off + nlen; k += 2u) {
                uint16_t c = get16(buf + k);

                if (c == 0)
                    break;
                if (c < 0x20u || c > 0x7Eu || n + 1u >= NXT_PATH_MAX) {
                    file_ok = 0;
                    break;
                }
                out->file[n++] = (char)c;
            }
        }
        off += nlen;
    }
    out->file[file_ok ? n : 0] = '\0';
    if (!file_ok)
        for (i = 0; i < NXT_PATH_MAX; i++)
            out->file[i] = '\0';
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
