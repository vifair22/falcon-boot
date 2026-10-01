/* falcon-boot: the firmware's TPM event log, replayed. See evlog.h. */

#include <tpm/evlog.h>

#include <crypto/sha1.h>
#include <crypto/sha256.h>
#include <lib/string.h>

#define SPEC_ID_SIG   "Spec ID Event03"
#define LOCALITY_SIG  "StartupLocality"
#define ALGS_MAX      8u

/* One TCG_PCR_EVENT header: pcr, type, SHA-1 digest, event size. */
#define EV12_HDR      32u

typedef struct {
    uint16_t alg;
    uint16_t size;
} alg_size;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int all_zero(const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (p[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

static void extend(evlog_replay *r, uint32_t pcr, const uint8_t *digest)
{
    uint8_t *v = r->pcr[pcr];

    if (r->alg == EVLOG_ALG_SHA1) {
        sha1_ctx h;

        sha1_init(&h);
        sha1_update(&h, v, SHA1_DIGEST_LEN);
        sha1_update(&h, digest, SHA1_DIGEST_LEN);
        sha1_final(&h, v);
    } else {
        sha256_ctx h;

        sha256_init(&h);
        sha256_update(&h, v, SHA256_DIGEST_LEN);
        sha256_update(&h, digest, SHA256_DIGEST_LEN);
        sha256_final(&h, v);
    }
}

/* An EV_NO_ACTION event is not extended; a StartupLocality one (TCG PC
 * Client Platform Firmware Profile Section 10.4.5.3) says the locality
 * TPM2_Startup ran at, which is PCR 0's starting value. */
static void no_action(evlog_replay *r, uint32_t pcr, const uint8_t *data,
                      uint32_t size)
{
    if (pcr == 0u && size == sizeof LOCALITY_SIG + 1u &&
        memcmp(data, LOCALITY_SIG, sizeof LOCALITY_SIG) == 0) {
        memset(r->pcr[0], 0, sizeof r->pcr[0]);
        r->pcr[0][r->digest_len - 1u] = data[sizeof LOCALITY_SIG];
    }
}

/* Handles one event's effect; nonzero when it was the stop event. */
static int apply(evlog_replay *r, uint32_t pcr, uint32_t type,
                 const uint8_t *digest, const uint8_t *data, uint32_t size,
                 unsigned int stop_pcr, const uint8_t *stop_digest)
{
    r->events++;
    if (type == EVLOG_EV_NO_ACTION) {
        no_action(r, pcr, data, size);
        return 0;
    }
    if (pcr >= EVLOG_PCRS) {
        return 0;
    }
    if (r->sub_on && pcr == r->sub_pcr &&
        memcmp(digest, r->sub_from, r->digest_len) == 0) {
        extend(r, pcr, r->sub_to);
        r->substituted++;
    } else {
        extend(r, pcr, digest);
    }
    return stop_digest != NULL && pcr == stop_pcr &&
           memcmp(digest, stop_digest, r->digest_len) == 0;
}

static int run12(const uint8_t *log, size_t len, unsigned int stop_pcr,
                 const uint8_t *stop_digest, evlog_replay *r)
{
    size_t off = 0;

    while (off < len) {
        uint32_t size;

        if (all_zero(log + off, len - off)) {
            break;
        }
        if (len - off < EV12_HDR) {
            return EVLOG_ERR_FORMAT;
        }
        size = le32(log + off + 28u);
        if (size > len - off - EV12_HDR) {
            return EVLOG_ERR_FORMAT;
        }
        if (apply(r, le32(log + off), le32(log + off + 4u), log + off + 8u,
                  log + off + EV12_HDR, size, stop_pcr, stop_digest)) {
            r->stopped = 1;
            break;
        }
        off += EV12_HDR + size;
    }
    return EVLOG_OK;
}

/* The Spec ID Event03's algorithm table (the first event's data): which
 * banks the log carries and each digest's size, which the events that
 * follow do not repeat. */
static int spec_id(const uint8_t *data, uint32_t size, alg_size *algs,
                   uint32_t *n)
{
    uint32_t i, count;

    /* signature 16, platformClass 4, version 3, uintnSize 1, count 4 */
    if (size < 28u) {
        return EVLOG_ERR_FORMAT;
    }
    count = le32(data + 24);
    if (count == 0u || count > ALGS_MAX || size < 28u + 4u * count) {
        return EVLOG_ERR_FORMAT;
    }
    for (i = 0; i < count; i++) {
        algs[i].alg = le16(data + 28u + (size_t)4 * i);
        algs[i].size = le16(data + 30u + (size_t)4 * i);
        if (algs[i].size == 0u || algs[i].size > 64u) {
            return EVLOG_ERR_FORMAT;
        }
    }
    *n = count;
    return EVLOG_OK;
}

static int run2(const uint8_t *log, size_t len, unsigned int stop_pcr,
                const uint8_t *stop_digest, evlog_replay *r)
{
    alg_size algs[ALGS_MAX];
    uint32_t nalg = 0;
    uint32_t i;
    size_t off;
    int have = 0;
    int rc;

    rc = spec_id(log + EV12_HDR, le32(log + 28u), algs, &nalg);
    if (rc != EVLOG_OK) {
        return rc;
    }
    for (i = 0; i < nalg; i++) {
        if (algs[i].alg == r->alg) {
            have = algs[i].size == r->digest_len;
        }
    }
    if (!have) {
        return EVLOG_ERR_BANK;
    }
    r->events++;
    off = EV12_HDR + le32(log + 28u);

    while (off < len) {
        const uint8_t *digest = NULL;
        uint32_t pcr, type, count, size;
        size_t at;

        if (all_zero(log + off, len - off)) {
            break;
        }
        if (len - off < 12u) {
            return EVLOG_ERR_FORMAT;
        }
        pcr = le32(log + off);
        type = le32(log + off + 4u);
        count = le32(log + off + 8u);
        if (count > ALGS_MAX) {
            return EVLOG_ERR_FORMAT;
        }
        at = off + 12u;
        for (i = 0; i < count; i++) {
            uint16_t alg;
            uint32_t k;
            size_t dlen = 0;

            if (len - at < 2u) {
                return EVLOG_ERR_FORMAT;
            }
            alg = le16(log + at);
            for (k = 0; k < nalg; k++) {
                if (algs[k].alg == alg) {
                    dlen = algs[k].size;
                }
            }
            if (dlen == 0u || len - at - 2u < dlen) {
                return EVLOG_ERR_FORMAT;
            }
            if (alg == r->alg) {
                digest = log + at + 2u;
            }
            at += 2u + dlen;
        }
        if (len - at < 4u) {
            return EVLOG_ERR_FORMAT;
        }
        size = le32(log + at);
        at += 4u;
        if (size > len - at) {
            return EVLOG_ERR_FORMAT;
        }
        /* Every measured event carries every bank the header names. */
        if (digest == NULL && type != EVLOG_EV_NO_ACTION) {
            return EVLOG_ERR_FORMAT;
        }
        if (apply(r, pcr, type, digest, log + at, size, stop_pcr,
                  stop_digest)) {
            r->stopped = 1;
            break;
        }
        off = at + size;
    }
    return EVLOG_OK;
}

int evlog_replay_init(uint16_t alg, evlog_replay *out)
{
    if (out == NULL) {
        return EVLOG_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    if (alg != EVLOG_ALG_SHA1 && alg != EVLOG_ALG_SHA256) {
        return EVLOG_ERR_ARG;
    }
    out->alg = alg;
    out->digest_len = alg == EVLOG_ALG_SHA1 ? SHA1_DIGEST_LEN
                                            : SHA256_DIGEST_LEN;
    return EVLOG_OK;
}

void evlog_replay_substitute(evlog_replay *r, unsigned int pcr,
                             const uint8_t *from, const uint8_t *to)
{
    if (r == NULL || from == NULL || to == NULL || pcr >= EVLOG_PCRS ||
        r->digest_len == 0u) {
        return;
    }
    r->sub_on = 1;
    r->sub_pcr = pcr;
    memcpy(r->sub_from, from, r->digest_len);
    memcpy(r->sub_to, to, r->digest_len);
}

int evlog_replay_more(const uint8_t *log, size_t len, unsigned int stop_pcr,
                      const uint8_t *stop_digest, evlog_replay *r)
{
    evlog_format f;

    if (r == NULL || r->digest_len == 0u || log == NULL || len < EV12_HDR) {
        return EVLOG_ERR_ARG;
    }
    r->stopped = 0;

    /* The crypto-agile log opens with a TCG_PCR_EVENT of type EV_NO_ACTION
     * whose data is the Spec ID Event03; a 1.2 log opens with a
     * measurement. */
    if (le32(log + 4u) == EVLOG_EV_NO_ACTION &&
        le32(log + 28u) >= sizeof SPEC_ID_SIG &&
        le32(log + 28u) <= len - EV12_HDR &&
        memcmp(log + EV12_HDR, SPEC_ID_SIG, sizeof SPEC_ID_SIG) == 0) {
        f = EVLOG_FMT_TCG2;
    } else {
        f = EVLOG_FMT_TCG12;
    }
    /* Two logs of one boot come from one TPM, so they share a format. */
    if (r->format != EVLOG_FMT_NONE && r->format != f) {
        return EVLOG_ERR_FORMAT;
    }
    r->format = f;
    if (f == EVLOG_FMT_TCG2) {
        return run2(log, len, stop_pcr, stop_digest, r);
    }
    if (r->alg != EVLOG_ALG_SHA1) {
        return EVLOG_ERR_BANK;
    }
    return run12(log, len, stop_pcr, stop_digest, r);
}

int evlog_replay_run(const uint8_t *log, size_t len, uint16_t alg,
                     unsigned int stop_pcr, const uint8_t *stop_digest,
                     evlog_replay *out)
{
    int rc = evlog_replay_init(alg, out);

    if (rc != EVLOG_OK) {
        return rc;
    }
    return evlog_replay_more(log, len, stop_pcr, stop_digest, out);
}
