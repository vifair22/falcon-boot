/* falcon-boot mkstick: the pure half of `station totp`. See totpcore.h. */

#include <mkstick/totpcore.h>

#include <stdio.h>
#include <string.h>

#include <crypto/sha1.h>
#include <tpm/evlog.h>
#include <tpm/evsave.h>

int totpc_parse_pcrs(const char *text, uint32_t *mask)
{
    uint32_t m = 0;
    const char *p = text;

    if (text == NULL || mask == NULL)
        return TOTPC_ERR_ARG;
    for (;;) {
        unsigned n = 0;
        int digits = 0;

        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            n = n * 10u + (unsigned)(*p - '0');
            p++;
            digits++;
        }
        while (*p == ' ')
            p++;
        if (digits == 0 || n >= STATIONINI_TOTP_PCRS || (m & (1u << n)))
            return TOTPC_ERR_ARG;
        m |= 1u << n;
        if (*p == '\0')
            break;
        if (*p != ',')
            return TOTPC_ERR_ARG;
        p++;
    }
    *mask = m;
    return TOTPC_OK;
}

int totpc_base32(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char al[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    size_t need = (n * 8u + 4u) / 5u;
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t i;
    size_t o = 0;

    if (in == NULL || out == NULL || cap < need + 1u)
        return TOTPC_ERR_SPACE;
    for (i = 0; i < n; i++) {
        acc = (acc << 8) | in[i];
        bits += 8u;
        while (bits >= 5u) {
            out[o++] = al[(acc >> (bits - 5u)) & 31u];
            bits -= 5u;
        }
    }
    if (bits > 0u)
        out[o++] = al[(acc << (5u - bits)) & 31u];
    out[o] = '\0';
    return TOTPC_OK;
}

/* RFC 3986 unreserved characters pass; everything else is %XX. */
static int pct(char *out, size_t cap, size_t *o, const char *s)
{
    static const char xd[] = "0123456789ABCDEF";

    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                    c == '_' || c == '~';

        if (*o + (plain ? 1u : 3u) >= cap)
            return TOTPC_ERR_SPACE;
        if (plain) {
            out[(*o)++] = (char)c;
        } else {
            out[(*o)++] = '%';
            out[(*o)++] = xd[c >> 4];
            out[(*o)++] = xd[c & 15u];
        }
    }
    out[*o] = '\0';
    return TOTPC_OK;
}

int totpc_uri(const char *account, const uint8_t *seed, size_t n, char *out,
              size_t cap)
{
    char secret[64];
    size_t o;
    int k;

    if (account == NULL || seed == NULL || out == NULL || n > 32u)
        return TOTPC_ERR_ARG;
    if (totpc_base32(seed, n, secret, sizeof secret) != TOTPC_OK)
        return TOTPC_ERR_SPACE;
    k = snprintf(out, cap, "otpauth://totp/" TOTP_ISSUER ":");
    if (k < 0 || (size_t)k >= cap)
        return TOTPC_ERR_SPACE;
    o = (size_t)k;
    if (pct(out, cap, &o, account) != TOTPC_OK)
        return TOTPC_ERR_SPACE;
    k = snprintf(out + o, cap - o, "?secret=%s&issuer=" TOTP_ISSUER
                 "&algorithm=SHA1&digits=6&period=30", secret);
    memset(secret, 0, sizeof secret);
    if (k < 0 || (size_t)k >= cap - o)
        return TOTPC_ERR_SPACE;
    return TOTPC_OK;
}

size_t totpc_trim(const uint8_t *p, size_t n)
{
    if (p == NULL)
        return 0;
    if (n > 0 && p[n - 1u] == '\n') {
        n--;
        if (n > 0 && p[n - 1u] == '\r')
            n--;
    }
    return n;
}

void totpc_owner12(const uint8_t *p, size_t n, uint8_t out[20])
{
    if (p == NULL) {
        memset(out, 0, 20);
        return;
    }
    if (n == 20u) {
        memcpy(out, p, 20);
        return;
    }
    sha1(p, totpc_trim(p, n), out);
}

int totpc_ini_update(const char *text, size_t len, const stationini_totp *t,
                     char *out, size_t cap, size_t *out_len,
                     unsigned *err_line, char *why)
{
    static stationini check;
    char sect[1024];
    size_t n;
    size_t m = 0;

    if (out == NULL || out_len == NULL || cap == 0 || err_line == NULL ||
        why == NULL || (text == NULL && len != 0))
        return TOTPC_ERR_ARG;
    *err_line = 0;
    why[0] = '\0';
    if (len != 0 && stationini_parse(text, len, &check) != 0) {
        *err_line = check.err_line;
        memcpy(why, check.why, STATIONINI_WHY_MAX);
        return TOTPC_ERR_INI;
    }
    n = len == 0 ? 0 : stationini_drop_section(text, len, "totp", out, cap);
    if (len == 0)
        out[0] = '\0';
    if (n == (size_t)-1)
        return TOTPC_ERR_SPACE;
    if (t != NULL) {
        m = stationini_format_totp(t, sect, sizeof sect);
        if (m == 0)
            return TOTPC_ERR_ARG;
        /* A blank line between the file and the section, and a newline
         * ending a last line that had none. */
        if (n > 0 && out[n - 1u] != '\n') {
            if (n + 1u >= cap)
                return TOTPC_ERR_SPACE;
            out[n++] = '\n';
        }
        if (n > 0 && (n < 2u || out[n - 2u] != '\n')) {
            if (n + 1u >= cap)
                return TOTPC_ERR_SPACE;
            out[n++] = '\n';
        }
        if (n + m >= cap)
            return TOTPC_ERR_SPACE;
        memcpy(out + n, sect, m + 1u);
        n += m;
    }
    out[n] = '\0';
    if (stationini_parse(out, n, &check) != 0 ||
        (t != NULL && !check.totp.present) ||
        (t == NULL && check.totp.present)) {
        *err_line = check.err_line;
        memcpy(why, check.why, STATIONINI_WHY_MAX);
        return TOTPC_ERR_INI;
    }
    *out_len = n;
    return TOTPC_OK;
}

/* The first PCR of 0-15 where the replay and the station's reading differ,
 * or EVSAVE_PCRS when none does. */
static unsigned first_diff(const evlog_replay *r, const uint8_t *pcr)
{
    unsigned i;

    for (i = 0; i < EVSAVE_PCRS; i++) {
        if (memcmp(r->pcr[i], pcr + (size_t)i * EVSAVE_DIGEST_LEN,
                   EVSAVE_DIGEST_LEN) != 0)
            return i;
    }
    return EVSAVE_PCRS;
}

int totpc_merge12(const uint8_t *fw, size_t fw_len, const uint8_t *rec,
                  size_t rec_len, const uint8_t station[20],
                  uint8_t out[16][20], unsigned *from, unsigned *bad_pcr)
{
    static evlog_replay r;
    evsave_view v;
    unsigned bad;

    if (rec == NULL || station == NULL || out == NULL || from == NULL ||
        bad_pcr == NULL)
        return TOTPC_ERR_ARG;
    if (evsave_read(rec, rec_len, &v) != EVSAVE_OK)
        return TOTPC_ERR_LOG;

    /* The station's own measurement, in the log it saved: that log is the
     * firmware's that started it. */
    if (evlog_replay_run(v.log, v.log_len, EVLOG_ALG_SHA1, 4, station, &r) !=
        EVLOG_OK)
        return TOTPC_ERR_LOG;
    if (!r.stopped)
        return TOTPC_ERR_STATION;

    /* One firmware log: the saved one is the whole story. */
    if (evlog_replay_run(v.log, v.log_len, EVLOG_ALG_SHA1, 0, NULL, &r) !=
        EVLOG_OK)
        return TOTPC_ERR_LOG;
    bad = first_diff(&r, v.pcr);
    if (bad == EVSAVE_PCRS) {
        *from = TOTPC_FROM_SAVED;
        memcpy(out, v.pcr, EVSAVE_PCR_BYTES);
        return TOTPC_OK;
    }

    /* Two: Linux's first, since what it holds ran before the payload whose
     * log the station saved (tpm/evlog.h). */
    if (fw != NULL && fw_len != 0) {
        if (evlog_replay_init(EVLOG_ALG_SHA1, &r) != EVLOG_OK ||
            evlog_replay_more(fw, fw_len, 0, NULL, &r) != EVLOG_OK ||
            evlog_replay_more(v.log, v.log_len, 0, NULL, &r) != EVLOG_OK)
            return TOTPC_ERR_LOG;
        bad = first_diff(&r, v.pcr);
        if (bad == EVSAVE_PCRS) {
            *from = TOTPC_FROM_BOTH;
            memcpy(out, v.pcr, EVSAVE_PCR_BYTES);
            return TOTPC_OK;
        }
    }
    *bad_pcr = bad;
    return TOTPC_ERR_PCR;
}
