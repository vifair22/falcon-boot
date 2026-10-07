/* falcon-boot: the TPM through the firmware. See tcg.h. */

#include <tcg/tcg.h>

#include <efi/tcg.h>
#include <crypto/crypto.h>
#include <crypto/sha256.h>
#include <lib/string.h>
#include <tpm/tpm12.h>
#include <tpm/tpm2.h>
#include <tsc/tsc.h>

static tpm_dev g_dev;
static EFI_TCG2_PROTOCOL *g_tcg2;
static EFI_TCG_PROTOCOL *g_tcg;

static uint8_t g_pool[SHA256_DIGEST_LEN];
static uint64_t g_ctr;

/* ---- the transport ---- */

/* The firmware writes the whole response into rsp; its header carries its
 * length, which tpm_transact checks against what it reads. The command
 * buffer is the library's and is const there; the protocols take it
 * non-const and do not write it. */
static int xfer(void *ctx, const uint8_t *cmd, size_t cmd_len, uint8_t *rsp,
                size_t rsp_cap, size_t *rsp_len)
{
    UINT8 *in = (UINT8 *)(uintptr_t)cmd;
    EFI_STATUS s;
    size_t n;

    (void)ctx;
    if (cmd_len > 0xFFFFFFFFu || rsp_cap < 10u || rsp_cap > 0xFFFFFFFFu)
        return -1;
    memset(rsp, 0, 10);
    if (g_tcg2 != (EFI_TCG2_PROTOCOL *)0)
        s = g_tcg2->SubmitCommand(g_tcg2, (UINT32)cmd_len, in,
                                  (UINT32)rsp_cap, rsp);
    else if (g_tcg != (EFI_TCG_PROTOCOL *)0)
        s = g_tcg->PassThroughToTpm(g_tcg, (UINT32)cmd_len, in,
                                    (UINT32)rsp_cap, rsp);
    else
        return -1;
    if (EFI_ERROR(s))
        return -1;
    n = ((size_t)rsp[2] << 24) | ((size_t)rsp[3] << 16) |
        ((size_t)rsp[4] << 8) | (size_t)rsp[5];
    if (n < 10u || n > rsp_cap)
        return -1;
    *rsp_len = n;
    return 0;
}

/* ---- randomness ---- */

static int rng(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    while (n > 0) {
        uint8_t block[SHA256_DIGEST_LEN];
        uint8_t ctr[8];
        sha256_ctx h;
        size_t k = n < sizeof block ? n : sizeof block;
        unsigned i;

        for (i = 0; i < 8u; i++)
            ctr[i] = (uint8_t)(g_ctr >> (8u * i));
        g_ctr++;
        sha256_init(&h);
        sha256_update(&h, g_pool, sizeof g_pool);
        sha256_update(&h, ctr, sizeof ctr);
        sha256_final(&h, block);
        memcpy(out, block, k);
        crypto_wipe(block, sizeof block);
        out += k;
        n -= k;
    }
    return 0;
}

static int have_rdrand(void)
{
    uint32_t a = 1, b, c = 0, d;

    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (int)((c >> 30) & 1u);
}

/* One RDRAND word; a failed one (carry clear) is retried a few times, as
 * Intel's guidance has it, and then given up on. */
static int rdrand64(uint64_t *v)
{
    int i;

    for (i = 0; i < 10; i++) {
        uint8_t ok;

        __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok)
            return 1;
    }
    return 0;
}

static void seed(EFI_SYSTEM_TABLE *st, tpm_version ver)
{
    EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *r = (EFI_RNG_PROTOCOL *)0;
    uint8_t buf[32];
    sha256_ctx h;
    uint64_t v;
    EFI_TIME t;
    unsigned i;

    memset(buf, 0, sizeof buf);
    sha256_init(&h);
    if (st->BootServices->LocateProtocol(&rng_guid, (void *)0,
                                         (void **)&r) == EFI_SUCCESS &&
        r != (EFI_RNG_PROTOCOL *)0 &&
        r->GetRNG(r, (EFI_GUID *)0, sizeof buf, buf) == EFI_SUCCESS)
        sha256_update(&h, buf, sizeof buf);
    if (have_rdrand()) {
        for (i = 0; i < 4u; i++) {
            if (rdrand64(&v))
                sha256_update(&h, &v, sizeof v);
        }
    }
    if ((ver == TPM_VER_20 && tpm2_get_random(&g_dev, buf, 32) == TPM_OK) ||
        (ver == TPM_VER_12 && tpm12_get_random(&g_dev, buf, 20) == TPM_OK))
        sha256_update(&h, buf, sizeof buf);
    memset(&t, 0, sizeof t);
    (void)st->RuntimeServices->GetTime(&t, (void *)0);
    sha256_update(&h, &t, sizeof t);
    v = tsc_read();
    sha256_update(&h, &v, sizeof v);
    sha256_final(&h, g_pool);
    crypto_wipe(buf, sizeof buf);
    v = 0;
    g_ctr = 0;
}

/* ---- open and close ---- */

tcg_status tcg_open(EFI_SYSTEM_TABLE *st, tpm_version want, tpm_dev **dev)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID g2 = EFI_TCG2_PROTOCOL_GUID;
    EFI_GUID g12 = EFI_TCG_PROTOCOL_GUID;

    g_tcg2 = (EFI_TCG2_PROTOCOL *)0;
    g_tcg = (EFI_TCG_PROTOCOL *)0;
    if (want == TPM_VER_20) {
        EFI_TCG2_BOOT_SERVICE_CAPABILITY cap;
        EFI_TCG2_PROTOCOL *p = (EFI_TCG2_PROTOCOL *)0;

        if (bs->LocateProtocol(&g2, (void *)0, (void **)&p) != EFI_SUCCESS ||
            p == (EFI_TCG2_PROTOCOL *)0)
            return TCG_NO_PROTOCOL;
        memset(&cap, 0, sizeof cap);
        cap.Size = (UINT8)sizeof cap;
        if (p->GetCapability(p, &cap) != EFI_SUCCESS || !cap.TPMPresentFlag)
            return TCG_NOT_PRESENT;
        g_tcg2 = p;
    } else {
        EFI_TCG_BOOT_SERVICE_CAPABILITY cap;
        EFI_TCG_PROTOCOL *p = (EFI_TCG_PROTOCOL *)0;
        UINT32 features = 0;
        UINT64 loc = 0;
        UINT64 last = 0;

        if (bs->LocateProtocol(&g12, (void *)0, (void **)&p) != EFI_SUCCESS ||
            p == (EFI_TCG_PROTOCOL *)0)
            return TCG_NO_PROTOCOL;
        memset(&cap, 0, sizeof cap);
        cap.Size = (UINT8)sizeof cap;
        if (p->StatusCheck(p, &cap, &features, &loc, &last) != EFI_SUCCESS ||
            !cap.TPMPresentFlag || cap.TPMDeactivatedFlag)
            return TCG_NOT_PRESENT;
        g_tcg = p;
    }

    memset(&g_dev, 0, sizeof g_dev);
    g_dev.xfer = xfer;
    g_dev.rng = rng;
    seed(st, want);
    *dev = &g_dev;
    return TCG_OK;
}

tcg_status tcg_evlog12(EFI_SYSTEM_TABLE *st, uint8_t *log, size_t cap,
                       size_t *len, uint8_t pcr[16][20])
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID g2 = EFI_TCG2_PROTOCOL_GUID;
    EFI_GUID g12 = EFI_TCG_PROTOCOL_GUID;
    EFI_TCG_BOOT_SERVICE_CAPABILITY cap12;
    EFI_TCG2_PROTOCOL *p2 = (EFI_TCG2_PROTOCOL *)0;
    EFI_TCG_PROTOCOL *p = (EFI_TCG_PROTOCOL *)0;
    const uint8_t *last_ev;
    UINT32 features = 0;
    UINT64 loc = 0;
    UINT64 last = 0;
    uint64_t n;
    unsigned i;

    if (bs->LocateProtocol(&g2, (void *)0, (void **)&p2) == EFI_SUCCESS &&
        p2 != (EFI_TCG2_PROTOCOL *)0)
        return TCG_NO_PROTOCOL;
    if (bs->LocateProtocol(&g12, (void *)0, (void **)&p) != EFI_SUCCESS ||
        p == (EFI_TCG_PROTOCOL *)0)
        return TCG_NO_PROTOCOL;
    memset(&cap12, 0, sizeof cap12);
    cap12.Size = (UINT8)sizeof cap12;
    if (p->StatusCheck(p, &cap12, &features, &loc, &last) != EFI_SUCCESS ||
        !cap12.TPMPresentFlag || cap12.TPMDeactivatedFlag)
        return TCG_NOT_PRESENT;

    /* A 1.2 log is TCG_PCR_EVENTs, 32 bytes of header and the event's
     * data; last is where the last one starts. */
    if (loc == 0 || last < loc || last - loc > cap)
        return TCG_NO_PROTOCOL;
    last_ev = (const uint8_t *)(uintptr_t)last;
    n = (last - loc) + 32u + ((uint64_t)last_ev[28] |
                              (uint64_t)last_ev[29] << 8 |
                              (uint64_t)last_ev[30] << 16 |
                              (uint64_t)last_ev[31] << 24);
    if (n > cap)
        return TCG_NO_PROTOCOL;
    memcpy(log, (const void *)(uintptr_t)loc, (size_t)n);
    *len = (size_t)n;

    /* The PCRs after the copy: an event logged in between would be in them
     * and not in the log, which enrollment then refuses rather than seal
     * to. No randomness is needed for a PCR read. */
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.xfer = xfer;
    g_tcg = p;
    for (i = 0; i < 16u; i++) {
        if (tpm12_pcr_read(&g_dev, i, pcr[i]) != TPM_OK) {
            tcg_close();
            return TCG_NOT_PRESENT;
        }
    }
    tcg_close();
    return TCG_OK;
}

tcg_status tcg_pcrs20(EFI_SYSTEM_TABLE *st, uint8_t pcr[16][32])
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_GUID g2 = EFI_TCG2_PROTOCOL_GUID;
    EFI_TCG2_BOOT_SERVICE_CAPABILITY cap;
    EFI_TCG2_PROTOCOL *p = (EFI_TCG2_PROTOCOL *)0;
    unsigned i;

    if (bs->LocateProtocol(&g2, (void *)0, (void **)&p) != EFI_SUCCESS ||
        p == (EFI_TCG2_PROTOCOL *)0)
        return TCG_NO_PROTOCOL;
    memset(&cap, 0, sizeof cap);
    cap.Size = (UINT8)sizeof cap;
    if (p->GetCapability(p, &cap) != EFI_SUCCESS || !cap.TPMPresentFlag)
        return TCG_NOT_PRESENT;

    /* No randomness is needed for a PCR read. */
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.xfer = xfer;
    g_tcg2 = p;
    for (i = 0; i < 16u; i++) {
        if (tpm2_pcr_read(&g_dev, i, pcr[i]) != TPM_OK) {
            tcg_close();
            return TCG_NOT_PRESENT;
        }
    }
    tcg_close();
    return TCG_OK;
}

void tcg_close(void)
{
    crypto_wipe(&g_dev, sizeof g_dev);
    crypto_wipe(g_pool, sizeof g_pool);
    g_ctr = 0;
    g_tcg2 = (EFI_TCG2_PROTOCOL *)0;
    g_tcg = (EFI_TCG_PROTOCOL *)0;
}
