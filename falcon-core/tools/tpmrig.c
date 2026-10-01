/* falcon-tpm-rig: falcon-core/src/tpm against swtpm, end to end.
 *
 * Purpose. The unit suite pins the marshalling to recorded exchanges; this
 * is where the recording comes from, and the proof that the session
 * cryptography is the TPM's reading of the specification and not only
 * ours: a real TPM (libtpms, under swtpm) either accepts the HMACs, the
 * salt and the policy, or it does not.
 *
 * What it does, per TPM version (DESIGN Section 12.15):
 * define the seed's NV index sealed to PCRs 0, 2, 4 and 7, write a 20-byte
 * seed, read it back through the encrypted path and compare, check that the
 * seed never appeared on the wire during that read (and, on 1.2, that it
 * does appear during a plain read, so the check can see it), refuse a
 * pinned key that does not match (2.0), extend PCR 7 and check the read is
 * refused as a PCR mismatch, and remove the index.
 *
 * Usage: falcon-tpm-rig --tpm2 SOCKET | --tpm12 SOCKET
 *                       [--record FILE [--prefix NAME]]
 * SOCKET is swtpm's unixio server socket. falcon-core/tools/run-tpm-rig.sh
 * starts swtpm and runs this; `make test-tpm` runs that.
 *
 * With --record the randomness is a fixed deterministic stream and every
 * exchange is written to FILE as C arrays, which is how
 * falcon-core/tests/tpm_transcripts.h is made: the unit suite replays the
 * same flow with the same stream and must send the same bytes. */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <crypto/sha1.h>
#include <crypto/sha256.h>
#include <tpm/tpm.h>
#include <tpm/tpm12.h>
#include <tpm/tpm2.h>

#define MAX_XCHG 64u

typedef struct {
    int      fd;
    FILE    *rec;
    unsigned n;
    /* The wire check: while `watch` is set, any command or response that
     * carries `secret` is counted. */
    int      watch;
    unsigned seen;
    const uint8_t *secret;
    size_t   secret_len;
} rig;

static int fails;
static const char *pfx = "k_";

static void check(int ok, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("%s ", ok ? "OK  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) {
        fails++;
    }
}

static int contains(const uint8_t *hay, size_t hn, const uint8_t *needle,
                    size_t nn)
{
    size_t i;

    for (i = 0; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) == 0) {
            return 1;
        }
    }
    return 0;
}

static int full_io(int fd, void *buf, size_t n, int wr)
{
    size_t done = 0;

    while (done < n) {
        ssize_t k = wr ? write(fd, (uint8_t *)buf + done, n - done)
                       : read(fd, (uint8_t *)buf + done, n - done);

        if (k < 0 && errno == EINTR) {
            continue;
        }
        if (k <= 0) {
            return -1;
        }
        done += (size_t)k;
    }
    return 0;
}

static void rec_array(FILE *f, const char *kind, unsigned n,
                      const uint8_t *b, size_t len)
{
    size_t i;

    fprintf(f, "static const uint8_t %s%s%u[%zu] = {", pfx, kind, n, len);
    for (i = 0; i < len; i++) {
        fprintf(f, "%s0x%02x,", (i % 12u) == 0 ? "\n    " : " ", b[i]);
    }
    fprintf(f, "\n};\n");
}

static int rig_xfer(void *ctx, const uint8_t *cmd, size_t cmd_len,
                    uint8_t *rsp, size_t rsp_cap, size_t *rsp_len)
{
    rig *r = ctx;
    uint8_t hdr[10];
    uint32_t size;

    if (full_io(r->fd, (void *)(uintptr_t)cmd, cmd_len, 1) != 0 ||
        full_io(r->fd, hdr, sizeof hdr, 0) != 0) {
        return -1;
    }
    size = ((uint32_t)hdr[2] << 24) | ((uint32_t)hdr[3] << 16) |
           ((uint32_t)hdr[4] << 8) | hdr[5];
    if (size < 10u || size > rsp_cap) {
        return -1;
    }
    memcpy(rsp, hdr, sizeof hdr);
    if (full_io(r->fd, rsp + 10, size - 10u, 0) != 0) {
        return -1;
    }
    *rsp_len = size;
    if (r->watch && (contains(cmd, cmd_len, r->secret, r->secret_len) ||
                     contains(rsp, size, r->secret, r->secret_len))) {
        r->seen++;
    }
    if (r->rec != NULL) {
        if (r->n >= MAX_XCHG) {
            return -1;
        }
        rec_array(r->rec, "cmd", r->n, cmd, cmd_len);
        rec_array(r->rec, "rsp", r->n, rsp, size);
        r->n++;
    }
    return 0;
}

/* The recording stream: SHA-256 of a counter, so the unit suite can
 * reproduce it byte for byte (falcon-core/tests/tpm_test.c, det_rng). */
typedef struct {
    uint32_t ctr;
    uint8_t  block[32];
    size_t   left;
} det;

static int det_rng(void *ctx, uint8_t *out, size_t n)
{
    det *g = ctx;

    while (n > 0) {
        size_t k;

        if (g->left == 0) {
            uint8_t c[4];

            c[0] = (uint8_t)(g->ctr >> 24);
            c[1] = (uint8_t)(g->ctr >> 16);
            c[2] = (uint8_t)(g->ctr >> 8);
            c[3] = (uint8_t)g->ctr;
            g->ctr++;
            sha256(c, 4, g->block);
            g->left = 32;
        }
        k = n < g->left ? n : g->left;
        memcpy(out, g->block + 32 - g->left, k);
        g->left -= k;
        out += k;
        n -= k;
    }
    return 0;
}

static int os_rng(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    return getrandom(out, n, 0) == (ssize_t)n ? 0 : -1;
}

static const uint8_t SEED[20] = {
    'f', 'a', 'l', 'c', 'o', 'n', ' ', 't', 'o', 't',
    'p', ' ', 's', 'e', 'e', 'd', ' ', 'r', 'i', 'g'
};

#define PCRS ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 7))

static void flow_tpm2(tpm_dev *d, rig *r)
{
    uint8_t name[TPM2_NAME_LEN], bad[TPM2_NAME_LEN], out[20], ext[32];
    tpm_version v;
    int rc;

    check(tpm2_startup(d) == TPM_OK, "tpm2: startup");
    rc = tpm_detect(d, &v);
    check(rc == TPM_OK && v == TPM_VER_20, "tpm2: detected as 2.0");
    (void)tpm2_nv_undefine(d, NULL, 0, TPM2_TOTP_INDEX);

    r->watch = 1;
    rc = tpm2_seed_enroll(d, NULL, 0, TPM2_TOTP_INDEX, PCRS, SEED,
                          sizeof SEED, name);
    r->watch = 0;
    check(rc == TPM_OK, "tpm2: enroll (define, sealed to PCRs 0,2,4,7; "
          "write) rc=%d tpm_rc=0x%x", rc, d->rc);
    check(r->seen == 0, "tpm2: the seed was not on the wire during the "
          "write (%u exchanges carried it)", r->seen);
    {
        unsigned k;

        printf("     tpm2: salt key name ");
        for (k = 0; k < TPM2_NAME_LEN; k++) {
            printf("%02x", name[k]);
        }
        printf("\n");
    }

    r->seen = 0;
    r->watch = 1;
    memset(out, 0, sizeof out);
    rc = tpm2_seed_read(d, TPM2_TOTP_INDEX, PCRS, name, out, sizeof out);
    r->watch = 0;
    check(rc == TPM_OK && memcmp(out, SEED, sizeof SEED) == 0,
          "tpm2: salted policy read returns the seed rc=%d tpm_rc=0x%x",
          rc, d->rc);
    check(r->seen == 0, "tpm2: the seed was not on the wire during the "
          "read (%u exchanges carried it)", r->seen);

    memcpy(bad, name, sizeof bad);
    bad[5] ^= 1u;
    rc = tpm2_seed_read(d, TPM2_TOTP_INDEX, PCRS, bad, out, sizeof out);
    check(rc == TPM_ERR_KEY, "tpm2: a salt key other than the pinned one "
          "is refused rc=%d", rc);

    /* The control: the same seed written without the encryption session
     * crosses the wire in the clear, and the check sees it. */
    r->seen = 0;
    r->watch = 1;
    rc = tpm2_nv_write(d, NULL, 0, TPM2_TOTP_INDEX, SEED, sizeof SEED, NULL);
    r->watch = 0;
    check(rc == TPM_OK && r->seen == 1, "tpm2: control: a write without "
          "the encryption session carries the seed and the wire check sees "
          "it (%u)", r->seen);

    memset(ext, 0x5a, sizeof ext);
    check(tpm2_pcr_extend(d, 7, ext) == TPM_OK, "tpm2: extend PCR 7");
    rc = tpm2_seed_read(d, TPM2_TOTP_INDEX, PCRS, name, out, sizeof out);
    check(rc == TPM_ERR_PCR, "tpm2: after the extend the read is refused "
          "as a PCR mismatch rc=%d tpm_rc=0x%x", rc, d->rc);

    rc = tpm2_nv_undefine(d, NULL, 0, TPM2_TOTP_INDEX);
    check(rc == TPM_OK, "tpm2: undefine rc=%d", rc);
    rc = tpm2_nv_name(d, TPM2_TOTP_INDEX, name, NULL);
    check(rc == TPM_ERR_RC, "tpm2: the index is gone rc=%d", rc);
}

static void flow_tpm12(tpm_dev *d, rig *r)
{
    /* swtpm_setup --owner-well-known and --srk-well-known take 20 zero
     * bytes as a password and store its SHA-1, TrouSerS' convention for a
     * password, where TrouSerS' own -z and Windows store the zeros
     * themselves. So both secrets here are SHA-1 of 20 zero bytes. */
    static const uint8_t zeros[20] = { 0 };
    uint8_t owner[20];
    uint8_t out[20], ext[20];
    tpm12_pubkey srk;
    tpm_version v;
    int owned = 0, locked = 0;
    int rc;

    sha1(zeros, sizeof zeros, owner);
    check(tpm12_startup(d) == TPM_OK, "tpm12: startup");
    rc = tpm_detect(d, &v);
    check(rc == TPM_OK && v == TPM_VER_12, "tpm12: detected as 1.2");
    rc = tpm12_flags(d, &owned, &locked);
    check(rc == TPM_OK && owned && locked, "tpm12: owned and NV locked "
          "(owned=%d nvLocked=%d)", owned, locked);
    (void)tpm12_nv_undefine(d, owner, TPM12_TOTP_INDEX);

    rc = tpm12_seed_enroll(d, owner, TPM12_TOTP_INDEX, PCRS, SEED,
                           sizeof SEED, &srk);
    check(rc == TPM_OK && srk.mod_len == 256u,
          "tpm12: enroll (SRK read, define sealed to PCRs 0,2,4,7, write) "
          "rc=%d tpm_rc=%u", rc, d->rc);

    r->seen = 0;
    r->watch = 1;
    rc = tpm12_nv_read(d, TPM12_TOTP_INDEX, out, sizeof out);
    r->watch = 0;
    check(rc == TPM_OK && memcmp(out, SEED, sizeof SEED) == 0 && r->seen == 1,
          "tpm12: control: a plain read returns the seed and the wire "
          "check sees it (%u)", r->seen);

    r->seen = 0;
    r->watch = 1;
    memset(out, 0, sizeof out);
    rc = tpm12_seed_read(d, TPM12_TOTP_INDEX, owner, &srk, out, sizeof out);
    r->watch = 0;
    check(rc == TPM_OK && memcmp(out, SEED, sizeof SEED) == 0,
          "tpm12: transport read returns the seed rc=%d tpm_rc=%u", rc, d->rc);
    check(r->seen == 0, "tpm12: the seed was not on the wire during the "
          "transport read (%u exchanges carried it)", r->seen);

    memset(ext, 0x5a, sizeof ext);
    check(tpm12_extend(d, 7, ext) == TPM_OK, "tpm12: extend PCR 7");
    rc = tpm12_seed_read(d, TPM12_TOTP_INDEX, owner, &srk, out, sizeof out);
    check(rc == TPM_ERR_PCR, "tpm12: after the extend the transport read is "
          "refused as a PCR mismatch rc=%d tpm_rc=%u", rc, d->rc);
    rc = tpm12_nv_read(d, TPM12_TOTP_INDEX, out, sizeof out);
    check(rc == TPM_ERR_PCR, "tpm12: and so is a plain read rc=%d", rc);

    rc = tpm12_nv_undefine(d, owner, TPM12_TOTP_INDEX);
    check(rc == TPM_OK, "tpm12: undefine rc=%d tpm_rc=%u", rc, d->rc);
    rc = tpm12_nv_read(d, TPM12_TOTP_INDEX, out, sizeof out);
    check(rc == TPM_ERR_RC, "tpm12: the index is gone rc=%d", rc);
}

int main(int argc, char **argv)
{
    static tpm_dev dev;
    struct sockaddr_un sa;
    rig r;
    det g;
    int v2 = -1;
    const char *sock = NULL, *recpath = NULL;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tpm2") == 0 && i + 1 < argc) {
            v2 = 1;
            sock = argv[++i];
        } else if (strcmp(argv[i], "--tpm12") == 0 && i + 1 < argc) {
            v2 = 0;
            sock = argv[++i];
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            recpath = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            pfx = argv[++i];
        } else {
            fprintf(stderr, "usage: %s --tpm2 SOCKET | --tpm12 SOCKET "
                    "[--record FILE [--prefix NAME]]\n", argv[0]);
            return 2;
        }
    }
    if (sock == NULL || strlen(sock) >= sizeof sa.sun_path) {
        fprintf(stderr, "falcon-tpm-rig: a socket path is required\n");
        return 2;
    }

    memset(&r, 0, sizeof r);
    r.secret = SEED;
    r.secret_len = sizeof SEED;
    r.fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, sock, strlen(sock));
    if (r.fd < 0 || connect(r.fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "falcon-tpm-rig: cannot connect to %s: %s\n", sock,
                strerror(errno));
        return 1;
    }
    if (recpath != NULL) {
        r.rec = fopen(recpath, "w");
        if (r.rec == NULL) {
            fprintf(stderr, "falcon-tpm-rig: cannot write %s\n", recpath);
            return 1;
        }
        fprintf(r.rec, "/* Recorded by falcon-tpm-rig --record against swtpm "
                "(%s); see falcon-core/tests/tpm_test.c. */\n",
                v2 ? "TPM 2.0" : "TPM 1.2");
    }

    dev.xfer = rig_xfer;
    dev.xfer_ctx = &r;
    memset(&g, 0, sizeof g);
    if (recpath != NULL) {
        dev.rng = det_rng;
        dev.rng_ctx = &g;
    } else {
        dev.rng = os_rng;
    }

    if (v2) {
        flow_tpm2(&dev, &r);
    } else {
        flow_tpm12(&dev, &r);
    }

    if (r.rec != NULL) {
        unsigned k;

        fprintf(r.rec, "static const xchg %sxchg[%u] = {\n", pfx, r.n);
        for (k = 0; k < r.n; k++) {
            fprintf(r.rec, "    { %scmd%u, sizeof %scmd%u, %srsp%u, "
                    "sizeof %srsp%u },\n", pfx, k, pfx, k, pfx, k, pfx, k);
        }
        fprintf(r.rec, "};\n");
        fclose(r.rec);
    }
    close(r.fd);
    printf("%s: %d failed\n", v2 ? "tpm2" : "tpm12", fails);
    return fails == 0 ? 0 : 1;
}
