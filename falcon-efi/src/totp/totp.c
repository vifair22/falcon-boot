/* falcon-boot: the UEFI station's TOTP check. See totp.h. */

#include <totp/totp.h>

#include <crypto/crypto.h>
#include <crypto/otp.h>
#include <crypto/sha1.h>
#include <lib/string.h>
#include <loader/loadops.h>
#include <tcg/tcg.h>
#include <tpm/evsave.h>
#include <tpm/tpm12.h>
#include <tpm/tpm2.h>
#include <tui/tui.h>

#define SEED_LEN 20u

typedef enum {
    ST_NONE = 0,        /* no [totp]: nothing is drawn */
    ST_OK,              /* the seed is held */
    ST_WIPED,           /* it was, until something else ran */
    ST_NO_TPM,
    ST_PCR,
    ST_KEY,
    ST_AUTH,
    ST_REFUSED,         /* another response code, kept in g_rc */
    ST_IO
} totp_state;

static totp_state g_state;
static uint8_t g_seed[SEED_LEN];
static uint32_t g_rc;
static uint32_t g_ord;              /* the command g_rc answered */
static uint8_t g_ver;               /* 12 or 20, for "No TPM 2.0 found." */
static char g_label[STATIONINI_LABEL_MAX];

/* The code for the step last computed, so a refresh inside the same step
 * costs a clock read and nothing else. */
static uint64_t g_step = (uint64_t)-1;
static uint32_t g_code;

#define ATTR_CODE  TUI_ATTR(TUI_WHITE, TUI_BLACK)
#define ATTR_LABEL TUI_ATTR(TUI_LIGHTGRAY, TUI_BLACK)
#define ATTR_WARN  TUI_ATTR(TUI_YELLOW, TUI_BLACK)

/* edk2's 1.2 log is a few kilobytes on the machines it has been read on
 * (the T430's, the TPM rig's OVMF); a firmware whose log does not fit, or
 * whose variable store takes no record this size, gets none, and
 * enrollment says so. */
#define EVLOG_CAP ((size_t)32 * 1024)

/* ---- the saved record's status line (tpm/evsave.h) ---- */

typedef struct {
    char   text[EVSAVE_STATUS_MAX];
    size_t len;
} sline;

static void sl_put(sline *l, const char *s)
{
    while (*s != '\0' && l->len < sizeof l->text)
        l->text[l->len++] = *s++;
}

static void sl_hex(sline *l, uint64_t v, unsigned digits)
{
    static const char xd[] = "0123456789abcdef";
    char s[19];
    unsigned i;

    s[0] = '0';
    s[1] = 'x';
    for (i = 0; i < digits; i++)
        s[2 + i] = xd[(v >> (4u * (digits - 1u - i))) & 0xFu];
    s[2 + digits] = '\0';
    sl_put(l, s);
}

static void sl_dec(sline *l, unsigned v)
{
    char s[12];
    unsigned i = sizeof s - 1u;

    s[i] = '\0';
    do {
        s[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u && i > 0u);
    sl_put(l, s + i);
}

/* Where a read for the record stopped, after "TPM 2.0: " or "TPM 1.2: ". */
static void sl_diag(sline *l, const tcg_diag *d)
{
    switch (d->step) {
    case TCG_STEP_CAPABILITY:
        sl_put(l, "the protocol's capability query returned ");
        sl_hex(l, (uint64_t)d->efi, 16);
        break;
    case TCG_STEP_PRESENT:
        sl_put(l, "the firmware says the TPM is not usable");
        break;
    case TCG_STEP_LOG:
        sl_put(l, "the event log is not where the firmware says, or is "
                  "larger than the station copies");
        break;
    case TCG_STEP_PCR_READ:
        sl_put(l, "reading PCR ");
        sl_dec(l, d->pcr);
        if (d->err == TPM_ERR_RC) {
            sl_put(l, ": the TPM answered ");
            sl_hex(l, d->rc, 8);
        } else if (d->err == TPM_ERR_IO) {
            sl_put(l, ": the protocol's command call returned ");
            sl_hex(l, (uint64_t)d->efi, 16);
        } else {
            sl_put(l, ": the answer did not parse");
        }
        break;
    default:
        sl_put(l, "no protocol");
        break;
    }
}

void totp_save_log(EFI_SYSTEM_TABLE *st)
{
    static uint8_t log[EVLOG_CAP];
    static uint8_t rec[EVSAVE_BODY + EVLOG_CAP];
    uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN];
    uint8_t pcr2[EVSAVE_PCRS][EVSAVE_DIGEST_LEN2];
    tcg_diag why2;
    tcg_diag why12;
    tcg_status ts;
    EFI_STATUS es;
    size_t len = 0;
    size_t n = 0;
    sline l;

    l.len = 0;
    ts = tcg_pcrs20(st, pcr2, &why2);
    if (ts == TCG_OK) {
        if (evsave_write2(rec, sizeof rec,
                          (const uint8_t (*)[EVSAVE_DIGEST_LEN2])pcr2,
                          &n) == EVSAVE_OK) {
            es = loader_evlog_set(st, rec, n);
            if (es == EFI_SUCCESS) {
                sl_put(&l, "saved: TPM 2.0, PCRs 0-15 in the SHA-256 bank");
            } else {
                sl_put(&l, "TPM 2.0: the PCRs were read, and writing "
                           "FalconEvLog returned ");
                sl_hex(&l, (uint64_t)es, 16);
            }
        }
    } else if (ts == TCG_NOT_PRESENT) {
        sl_put(&l, "TPM 2.0: ");
        sl_diag(&l, &why2);
    } else if (tcg_evlog12(st, log, sizeof log, &len, pcr, &why12) ==
               TCG_OK) {
        if (evsave_write(rec, sizeof rec,
                         (const uint8_t (*)[EVSAVE_DIGEST_LEN])pcr, log, len,
                         &n) == EVSAVE_OK) {
            es = loader_evlog_set(st, rec, n);
            if (es == EFI_SUCCESS) {
                sl_put(&l, "saved: TPM 1.2, PCRs 0-15 and the event log, ");
                sl_dec(&l, (unsigned)len);
                sl_put(&l, " bytes");
            } else {
                sl_put(&l, "TPM 1.2: the PCRs and the log were read, and "
                           "writing FalconEvLog returned ");
                sl_hex(&l, (uint64_t)es, 16);
            }
        }
    } else if (why12.step == TCG_STEP_LOCATE) {
        sl_put(&l, "no TPM: the firmware offers neither EFI_TCG2_PROTOCOL "
                   "nor EFI_TCG_PROTOCOL");
    } else {
        sl_put(&l, "TPM 1.2: ");
        sl_diag(&l, &why12);
    }
    if (l.len == 0)
        sl_put(&l, "the record did not fit the station's buffer");
    (void)loader_evlog_status_set(st, l.text, l.len);
}

void totp_load(EFI_SYSTEM_TABLE *st, const stationini_totp *cfg)
{
    tpm_dev *dev = (tpm_dev *)0;
    tcg_status ts;
    int rc;

    g_state = ST_NONE;
    if (cfg == (const stationini_totp *)0 || !cfg->present)
        return;
    memcpy(g_label, cfg->label, sizeof g_label);
    g_label[sizeof g_label - 1u] = '\0';
    g_ver = cfg->tpm;

    ts = tcg_open(st, cfg->tpm == 20 ? TPM_VER_20 : TPM_VER_12, &dev);
    if (ts != TCG_OK) {
        g_state = ST_NO_TPM;
        return;
    }
    if (cfg->tpm == 20) {
        rc = tpm2_seed_read(dev, cfg->index, cfg->pcrs, cfg->key, g_seed,
                            SEED_LEN);
    } else {
        tpm12_pubkey pk;

        memset(&pk, 0, sizeof pk);
        memcpy(pk.mod, cfg->key, cfg->key_len);
        pk.mod_len = cfg->key_len;
        pk.exp[0] = 0x01;
        pk.exp[1] = 0x00;
        pk.exp[2] = 0x01;
        pk.exp_len = 3;
        static const uint8_t zeros[20] = { 0 };
        uint8_t srk_auth[20];
        unsigned tries;

        if (cfg->srk_sha1)
            sha1(zeros, sizeof zeros, srk_auth);
        else
            memset(srk_auth, 0, sizeof srk_auth);
        /* A firmware that sent TPM_Startup and not TPM_ContinueSelfTest
         * leaves the RSA engine untested, and EstablishTransport, the first
         * command that needs it, answers TPM_NEEDS_SELFTEST (the T430's
         * coreboot and edk2; Linux's driver sends the command, which is why
         * the same read works from there). So it is sent here, and a
         * command the TPM is not ready for is tried again for a few seconds
         * while the tests run. */
        (void)tpm12_continue_self_test(dev);
        for (tries = 0;; tries++) {
            rc = tpm12_seed_read(dev, cfg->index, srk_auth, &pk, g_seed,
                                 SEED_LEN);
            if (rc != TPM_ERR_RC || !tpm12_rc_retry(dev->rc) ||
                tries == 30u)
                break;
            if (dev->rc == TPM12_RC_NEEDS_SELFTEST)
                (void)tpm12_continue_self_test(dev);
            st->BootServices->Stall(100000);
        }
    }
    g_rc = dev->rc;
    g_ord = dev->ord;
    tcg_close();

    switch (rc) {
    case TPM_OK:
        g_state = ST_OK;
        return;
    case TPM_ERR_PCR:
        g_state = ST_PCR;
        break;
    case TPM_ERR_KEY:
        g_state = ST_KEY;
        break;
    case TPM_ERR_AUTH:
        g_state = ST_AUTH;
        break;
    case TPM_ERR_RC:
        g_state = ST_REFUSED;
        break;
    default:
        g_state = ST_IO;
        break;
    }
    crypto_wipe(g_seed, sizeof g_seed);
}

BOOLEAN totp_configured(void)
{
    return g_state != ST_NONE;
}

void totp_wipe(void)
{
    crypto_wipe(g_seed, sizeof g_seed);
    g_code = 0;
    g_step = (uint64_t)-1;
    if (g_state == ST_OK)
        g_state = ST_WIPED;
}

/* ---- the lines ---- */

typedef struct {
    CHAR16 *at;
    UINTN   left;
} wbuf;

static void put(wbuf *w, const char *s)
{
    while (*s != '\0' && w->left > 1u) {
        *w->at++ = (CHAR16)(unsigned char)*s++;
        w->left--;
    }
    *w->at = 0;
}

static void put_hex32(wbuf *w, uint32_t v)
{
    static const char xd[] = "0123456789abcdef";
    char s[11];
    unsigned i;

    s[0] = '0';
    s[1] = 'x';
    for (i = 0; i < 8u; i++)
        s[2 + i] = xd[(v >> (28u - 4u * i)) & 0xFu];
    s[10] = '\0';
    put(w, s);
}

/* Firmware time as seconds since the epoch, taken as UTC whatever time zone
 * it names: the design's choice (DESIGN Section 12.15), and
 * what an authenticator on a phone compares against. */
static int clock_now(EFI_SYSTEM_TABLE *st, uint64_t *now)
{
    EFI_TIME t;

    memset(&t, 0, sizeof t);
    if (st->RuntimeServices->GetTime(&t, (void *)0) != EFI_SUCCESS)
        return -1;
    return otp_unix_time(t.Year, t.Month, t.Day, t.Hour, t.Minute,
                         t.Second, now) == CRYPTO_OK ? 0 : -1;
}

static void line_code(EFI_SYSTEM_TABLE *st, wbuf *w, uint8_t *attr)
{
    char digits[8];
    uint64_t now = 0;
    uint64_t step;
    uint32_t c;
    int i;

    *attr = ATTR_WARN;
    if (clock_now(st, &now) != 0) {
        put(w, "The firmware clock could not be read.");
        return;
    }
    step = now / TOTP_STEP;
    if (step != g_step) {
        if (totp_sha1(g_seed, SEED_LEN, now, TOTP_STEP, TOTP_DIGITS,
                      &g_code) != CRYPTO_OK) {
            put(w, "The code could not be computed.");
            return;
        }
        g_step = step;
    }
    c = g_code;
    for (i = 6; i >= 4; i--) {
        digits[i] = (char)('0' + c % 10u);
        c /= 10u;
    }
    digits[3] = ' ';
    for (i = 2; i >= 0; i--) {
        digits[i] = (char)('0' + c % 10u);
        c /= 10u;
    }
    digits[7] = '\0';
    *attr = ATTR_CODE;
    put(w, "TOTP: ");
    put(w, digits);
}

void totp_status(void *ctx, tui_status *out)
{
    EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)ctx;
    wbuf w;
    wbuf w2;

    out->n = 0;
    if (g_state == ST_NONE)
        return;
    out->n = 1;
    out->attr[0] = ATTR_WARN;
    w.at = out->line[0];
    w.left = TUI_STATUS_MAX;
    w.at[0] = 0;
    switch (g_state) {
    case ST_OK:
        line_code(st, &w, &out->attr[0]);
        /* Which enrollment the code is, on a line of its own. */
        if (g_label[0] != '\0' && out->attr[0] == ATTR_CODE) {
            w2.at = out->line[1];
            w2.left = TUI_STATUS_MAX;
            w2.at[0] = 0;
            put(&w2, "(");
            put(&w2, g_label);
            put(&w2, ")");
            out->attr[1] = ATTR_LABEL;
            out->n = 2;
        }
        break;
    case ST_WIPED:
        put(&w, "The TOTP code is shown again after a restart.");
        break;
    case ST_NO_TPM:
        put(&w, g_ver == 20 ? "No TPM 2.0 found." : "No TPM 1.2 found.");
        break;
    case ST_PCR:
        put(&w, "The TPM did not release the code:");
        w2.at = out->line[1];
        w2.left = TUI_STATUS_MAX;
        put(&w2, "firmware, its settings or the boot chain changed since "
                 "enrollment.");
        out->attr[1] = ATTR_WARN;
        out->n = 2;
        break;
    case ST_KEY:
        put(&w, "The TPM key is not the enrolled one.");
        break;
    case ST_AUTH:
        put(&w, "The TPM's answer did not verify.");
        break;
    case ST_REFUSED:
        put(&w, "The TPM did not release the code: TPM error ");
        put_hex32(&w, g_rc);
        /* Which step refused, on 1.2, where the read is several commands
         * and the code alone does not say which of them it answered. */
        if (g_ver == 12 && tpm12_ord_name(g_ord) != (const char *)0) {
            put(&w, " in ");
            put(&w, tpm12_ord_name(g_ord));
        }
        put(&w, ".");
        break;
    default:
        put(&w, "The TPM could not be read.");
        break;
    }
}

