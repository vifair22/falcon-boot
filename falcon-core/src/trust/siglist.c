#include <trust/siglist.h>

#include <lib/string.h>

/* EFI_SIGNATURE_LIST: SignatureType (GUID), SignatureListSize,
 * SignatureHeaderSize, SignatureSize, then the header, then the entries.
 * Each EFI_SIGNATURE_DATA starts with a 16-byte owner GUID. */
#define LIST_HDR    28u
#define OWNER_LEN   16u
#define SHA256_LEN  32u

/* EFI_CERT_X509_GUID a5c059a1-94e4-4aa7-87b5-ab155c2bf072 and
 * EFI_CERT_SHA256_GUID c1c41626-504c-4092-aca9-41f936934328, in their
 * little-endian on-disk form. */
static const uint8_t GUID_X509[16] = {
    0xa1, 0x59, 0xc0, 0xa5, 0xe4, 0x94, 0xa7, 0x4a,
    0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72
};
static const uint8_t GUID_SHA256[16] = {
    0x26, 0x16, 0xc4, 0xc1, 0x4c, 0x50, 0x92, 0x40,
    0xac, 0xa9, 0x41, 0xf9, 0x36, 0x93, 0x43, 0x28
};

void siglist_begin(siglist_iter *it, der_span store)
{
    if (it == (siglist_iter *)0)
        return;
    memset(it, 0, sizeof(*it));
    it->rest = store;
    if (it->rest.p == (const uint8_t *)0)
        it->rest.len = 0;
}

/* Opens the next list into it->entries. 1 on success, 0 at the end, DER_ERR
 * on sizes that do not add up. */
static int open_list(siglist_iter *it)
{
    const uint8_t *p = it->rest.p;
    uint32_t list_size, hdr_size, sig_size;

    if (it->rest.len == 0)
        return 0;
    if (it->rest.len < LIST_HDR)
        return DER_ERR;
    list_size = get_le32(p + 16);
    hdr_size = get_le32(p + 20);
    sig_size = get_le32(p + 24);
    if (list_size < LIST_HDR || list_size > it->rest.len ||
        hdr_size > list_size - LIST_HDR || sig_size <= OWNER_LEN ||
        (list_size - LIST_HDR - hdr_size) % sig_size != 0)
        return DER_ERR;

    if (memcmp(p, GUID_X509, 16) == 0)
        it->type = SIG_X509;
    else if (memcmp(p, GUID_SHA256, 16) == 0 &&
             sig_size == OWNER_LEN + SHA256_LEN)
        it->type = SIG_SHA256;
    else
        it->type = SIG_OTHER;
    it->sig_size = sig_size;
    it->entries.p = p + LIST_HDR + hdr_size;
    it->entries.len = list_size - LIST_HDR - hdr_size;
    it->rest.p += list_size;
    it->rest.len -= list_size;
    return 1;
}

int siglist_next(siglist_iter *it, sig_type *type, der_span *data)
{
    if (it == (siglist_iter *)0 || type == (sig_type *)0 ||
        data == (der_span *)0)
        return DER_ERR;
    while (it->entries.len == 0) {
        int rc = open_list(it);

        if (rc != 1) {
            /* A bad list poisons the rest of the variable: its successor
             * would start at an offset computed from the bad size. */
            it->rest.len = 0;
            return rc;
        }
    }
    *type = it->type;
    data->p = it->entries.p + OWNER_LEN;
    data->len = it->sig_size - OWNER_LEN;
    it->entries.p += it->sig_size;
    it->entries.len -= it->sig_size;
    return 1;
}

int siglist_has_hash(der_span store, const uint8_t hash[32])
{
    siglist_iter it;
    sig_type type;
    der_span d;

    if (hash == (const uint8_t *)0)
        return 0;
    siglist_begin(&it, store);
    while (siglist_next(&it, &type, &d) == 1) {
        if (type == SIG_SHA256 && memcmp(d.p, hash, SHA256_LEN) == 0)
            return 1;
    }
    return 0;
}

/* ---- SBAT ---- */

/* One CSV line's first two fields: the component name and its generation.
 * Returns 1 when both are present and the generation is a decimal number. */
typedef struct {
    const uint8_t *name;
    size_t name_len;
    uint32_t gen;
} sbat_entry;

static int sbat_line(const uint8_t *p, size_t len, sbat_entry *e)
{
    size_t i = 0;
    uint32_t gen = 0;
    size_t digits = 0;

    while (i < len && p[i] != ',')
        i++;
    if (i == 0 || i == len)
        return 0;
    e->name = p;
    e->name_len = i;
    for (i++; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
        if (gen > 100000000u)
            return 0;   /* no real generation is this large */
        gen = gen * 10u + (uint32_t)(p[i] - '0');
        digits++;
    }
    if (digits == 0 || (i < len && p[i] != ','))
        return 0;
    e->gen = gen;
    return 1;
}

/* Splits `text` at the next newline, dropping a trailing CR and stopping at a
 * NUL (SbatLevelRT is commonly NUL-terminated). */
static int next_line(der_span *text, const uint8_t **line, size_t *len)
{
    size_t i = 0;

    if (text->len == 0 || text->p[0] == '\0')
        return 0;
    while (i < text->len && text->p[i] != '\n' && text->p[i] != '\0')
        i++;
    *line = text->p;
    *len = i;
    if (i > 0 && text->p[i - 1u] == '\r')
        (*len)--;
    if (i < text->len && text->p[i] == '\n')
        i++;
    text->p += i;
    text->len -= i;
    return 1;
}

int sbat_revoked(der_span level, der_span img)
{
    const uint8_t *il;
    size_t ilen;

    if (level.p == (const uint8_t *)0 || img.p == (const uint8_t *)0)
        return 0;
    while (next_line(&img, &il, &ilen)) {
        sbat_entry ie;
        der_span lv = level;
        const uint8_t *ll;
        size_t llen;

        if (!sbat_line(il, ilen, &ie))
            continue;
        while (next_line(&lv, &ll, &llen)) {
            sbat_entry le;

            if (sbat_line(ll, llen, &le) && le.name_len == ie.name_len &&
                memcmp(le.name, ie.name, ie.name_len) == 0 &&
                ie.gen < le.gen)
                return 1;
        }
    }
    return 0;
}
