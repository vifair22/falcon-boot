/* falcon-boot mkstick: `station totp enroll|reseal|remove`, the UEFI
 * station's TOTP check (docs/DESIGN.md Section 13.15).
 *
 * Enrollment runs on the station's machine, from a system the station
 * started, and talks to the TPM through the kernel (/dev/tpmrm0 for a TPM
 * 2.0, /dev/tpm0 for a 1.2) with falcon-core's tpm/, the same command layer
 * the station reads the seed with. It seals a fresh 20-byte seed into the
 * TPM's NV storage under the PCR values the station runs with, records the
 * TPM version, the index, the PCRs and the pinned TPM key in STATION.INI's
 * [totp], and prints the secret once, as an otpauth URI and, when qrencode
 * is installed, as a QR code.
 *
 * The PCR values are not the ones the TPM holds now: by the time Linux runs,
 * the station has loaded a UKI and moved PCR 4 (and shim PCR 7) on. They
 * come from the firmware's event log instead, replayed up to the event that
 * measured the station's own image (falcon-core/src/tpm/evlog.h), and the
 * log is first checked to account for the PCRs as they are now. Finding the
 * station's measurement in the log is also the proof that this system was
 * started by the station on the named disk; without it enrollment refuses.
 *
 * Reseal is enrollment again over an existing one: the seed cannot be read
 * back from this system (its PCRs are not the station's), so a new one is
 * made and the authenticator's entry is replaced. Remove deletes the index
 * and the section.
 *
 * Two options are for the TPM rig only (falcon-efi/tools/run-tpm-rig.sh):
 * --tpm-socket talks to swtpm's socket instead of a device, and --event-log
 * reads the log from a file (the one the rig's probe saved in the VM). With
 * --tpm-socket the TPM is not the one that booted, so the log is not
 * checked against its PCRs.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <common/log.h>
#include <common/proc.h>
#include <common/util.h>
#include <mkstick/mkstick.h>
#include <mkstick/station.h>
#include <mkstick/totpcore.h>

#include <crypto/crypto.h>
#include <crypto/hash.h>
#include <crypto/sha1.h>
#include <fs/fs.h>
#include <ident/ident.h>
#include <media/stationini.h>
#include <tpm/evlog.h>
#include <tpm/tpm.h>
#include <tpm/tpm12.h>
#include <tpm/tpm2.h>
#include <trust/authenticode.h>

#define STATION_EFI      "grubx64.efi"
#define EFI_MAX          ((size_t)16 * 1024 * 1024)
#define LOG_MAX          ((size_t)4 * 1024 * 1024)
#define INI_MAX          16384u
#define OWNER_MAX        1024u
#define SECURITYFS_LOG   "/sys/kernel/security/tpm0/binary_bios_measurements"

/* ---- the station on the disk ---- */

typedef struct {
    stn_scan  s;
    uint64_t  off;              /* the ESP, in bytes */
    uint64_t  len;
    uint32_t  number;
    int       mounted;
    char      where[MKS_PATH_MAX];
    char      ini[INI_MAX];
    size_t    ini_len;
    int       have_ini;
    stationini parsed;
} stn_esp;

/* A file under \EFI\falcon: through the mount when the ESP is mounted, off
 * the partition with core's FAT driver when it is not. */
static cli_status esp_read(stn_esp *e, const char *name, void *buf,
                           size_t cap, size_t *got)
{
    char path[MKS_PATH_MAX + 64];
    int rc;

    if (e->mounted) {
        void *data = NULL;
        size_t len = 0;
        cli_status st;

        (void)snprintf(path, sizeof path, "%s/%s/%s", e->where, STN_ESP_DIR,
                       name);
        if (!util_is_file(path))
            return CLI_ERR_NOTFOUND;
        st = util_read_file(path, &data, &len, cap - 1u);
        if (st != CLI_OK)
            return st;
        memcpy(buf, data, len);
        ((char *)buf)[len] = '\0';
        free(data);
        *got = len;
        return CLI_OK;
    }
    (void)snprintf(path, sizeof path, "/%s/%s", STN_ESP_DIR, name);
    rc = stn_read_file(e->s.disks[0].d.path, e->off, e->len, path, buf, cap,
                       got);
    if (rc == FS_OK)
        return CLI_OK;
    return rc == FS_ERR_RANGE ? CLI_ERR_RANGE : CLI_ERR_NOTFOUND;
}

static cli_status esp_write_ini(stn_esp *e, const char *text, size_t len)
{
    char path[MKS_PATH_MAX + 64];

    if (e->mounted) {
        (void)snprintf(path, sizeof path, "%s/%s/%s", e->where, STN_ESP_DIR,
                       STATIONINI_FILE);
        return util_write_file(path, text, len, 0644);
    }
    (void)snprintf(path, sizeof path, "%s/%s", STN_ESP_DIR, STATIONINI_FILE);
    return stn_mtools_put_text(e->s.disks[0].d.path, e->off, text, len,
                               path);
}

static cli_status esp_open(stn_esp *e, const char *disk)
{
    const mks_records *r;
    const part_entry *p;
    int i;
    cli_status rc;

    memset(e, 0, sizeof *e);
    rc = stn_scan_disks(&e->s, disk, NULL, 0, 1);
    if (rc != CLI_OK)
        return rc;
    r = &e->s.disks[0].r;
    if (r->rc != PART_OK)
        return set_error(CLI_ERR_INVALID, "%s has no usable partition table",
                         disk);
    i = stn_find_esp(r);
    if (i < 0 || r->recs[i].state != REC_OK ||
        r->recs[i].rec.role != IDENT_ROLE_BOOT)
        return set_error(CLI_ERR_NOTFOUND, "%s has no ESP carrying a "
                         "station's boot record: the UEFI station is not "
                         "installed there", disk);
    p = &r->parts[i];
    e->number = p->number;
    e->off = p->start_lba * e->s.disks[0].d.sector;
    e->len = (p->end_lba - p->start_lba + 1u) * e->s.disks[0].d.sector;
    e->mounted = stn_part_mounted(&e->s.disks[0].d, p->number, e->where,
                                  sizeof e->where);
    rc = esp_read(e, STATIONINI_FILE, e->ini, sizeof e->ini, &e->ini_len);
    if (rc == CLI_OK) {
        e->have_ini = 1;
        if (stationini_parse(e->ini, e->ini_len, &e->parsed) != 0)
            return set_error(CLI_ERR_INVALID, "\\EFI\\falcon\\%s line %u: "
                             "%s; fix it first", STATIONINI_FILE,
                             e->parsed.err_line, e->parsed.why);
    } else if (rc != CLI_ERR_NOTFOUND) {
        return set_error(rc, "\\EFI\\falcon\\%s could not be read",
                         STATIONINI_FILE);
    }
    cli_error_clear();
    return CLI_OK;
}

/* ---- the TPM ---- */

typedef struct {
    int fd;
} tpm_fd;

static int full_write(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t k = write(fd, p, n);

        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* A device answers a command in one read of the whole response (a shorter
 * read loses the rest), a socket in as many as it takes; reading into the
 * whole buffer until the header's length has arrived serves both. */
static int fd_xfer(void *ctx, const uint8_t *cmd, size_t cmd_len,
                   uint8_t *rsp, size_t rsp_cap, size_t *rsp_len)
{
    tpm_fd *t = ctx;
    size_t got = 0;
    size_t want = 10;

    if (full_write(t->fd, cmd, cmd_len) != 0)
        return -1;
    while (got < want) {
        ssize_t k = read(t->fd, rsp + got, rsp_cap - got);

        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        got += (size_t)k;
        if (got >= 6) {
            want = ((size_t)rsp[2] << 24) | ((size_t)rsp[3] << 16) |
                   ((size_t)rsp[4] << 8) | rsp[5];
            if (want < 10u || want > rsp_cap)
                return -1;
        }
    }
    *rsp_len = got;
    return 0;
}

static int getrandom_rng(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    while (n > 0) {
        ssize_t k = getrandom(out, n, 0);

        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        out += k;
        n -= (size_t)k;
    }
    return 0;
}

static tpm_dev g_dev;
static tpm_fd g_fd = { -1 };

static cli_status tpm_open(const char *sock, tpm_version *ver)
{
    const char *path = NULL;

    if (sock != NULL) {
        struct sockaddr_un sa;

        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (strlen(sock) >= sizeof sa.sun_path)
            return set_error(CLI_ERR_USAGE, "--tpm-socket: path too long");
        memcpy(sa.sun_path, sock, strlen(sock));
        g_fd.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (g_fd.fd < 0 ||
            connect(g_fd.fd, (struct sockaddr *)&sa, sizeof sa) != 0)
            return set_error_errno(CLI_ERR_IO, "connecting to %s", sock);
    } else {
        path = access("/dev/tpmrm0", F_OK) == 0 ? "/dev/tpmrm0"
                                                : "/dev/tpm0";
        g_fd.fd = open(path, O_RDWR | O_CLOEXEC);
        if (g_fd.fd < 0)
            return errno == ENOENT
                       ? set_error(CLI_ERR_NOTFOUND, "this machine has no "
                                   "TPM the kernel knows (no /dev/tpmrm0 or "
                                   "/dev/tpm0)")
                       : set_error_errno(CLI_ERR_PERM, "opening %s", path);
    }
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.xfer = fd_xfer;
    g_dev.xfer_ctx = &g_fd;
    g_dev.rng = getrandom_rng;
    if (tpm_detect(&g_dev, ver) != TPM_OK)
        return set_error(CLI_ERR_IO, "%s did not answer as a TPM",
                         sock != NULL ? sock : path);
    return CLI_OK;
}

static void tpm_close(void)
{
    if (g_fd.fd >= 0)
        (void)close(g_fd.fd);
    g_fd.fd = -1;
    crypto_wipe(&g_dev, sizeof g_dev);
}

/* What a failed TPM step says. */
static cli_status tpm_fail(int rc, const char *what)
{
    switch (rc) {
    case TPM_ERR_RC:
        return set_error(CLI_ERR_IO, "%s: the TPM refused, code 0x%08x%s",
                         what, g_dev.rc,
                         (g_dev.rc & 0xFFFu) == 0x9A2u ||
                                 g_dev.rc == 0x1u
                             ? " (the owner authorisation is wrong: "
                               "--owner-auth-file)" : "");
    case TPM_ERR_NVLOCK:
        return set_error(CLI_ERR_INVALID, "%s: this TPM 1.2's NV storage is "
                         "not locked, so it would release the seed whatever "
                         "the PCRs say", what);
    case TPM_ERR_KEY:
        return set_error(CLI_ERR_INVALID, "%s: the TPM's storage key is not "
                         "usable, or its secret is not the well-known one",
                         what);
    default:
        return set_error(CLI_ERR_IO, "%s: TPM status %d", what, rc);
    }
}

/* ---- the owner's authorisation ---- */

typedef struct {
    uint8_t v[OWNER_MAX];
    size_t  n;
    uint8_t v12[20];
} owner_auth;

static cli_status owner_read(const char *file, owner_auth *o)
{
    void *data = NULL;
    size_t len = 0;
    cli_status rc;

    memset(o, 0, sizeof *o);
    if (file == NULL) {
        totpc_owner12(NULL, 0, o->v12);
        return CLI_OK;
    }
    rc = util_read_file(file, &data, &len, OWNER_MAX);
    if (rc != CLI_OK)
        return rc;
    o->n = totpc_trim(data, len);
    memcpy(o->v, data, o->n);
    totpc_owner12(data, len, o->v12);
    crypto_wipe(data, len);
    free(data);
    return CLI_OK;
}

/* ---- the event log ---- */

static cli_status read_log(const char *file, uint8_t **out, size_t *len)
{
    const char *path = file != NULL ? file : SECURITYFS_LOG;
    uint8_t *buf;
    size_t n = 0;
    int fd;

    /* securityfs reports a size of 0, so this reads to the end rather
     * than by the size. */
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT && file == NULL
                   ? set_error(CLI_ERR_NOTFOUND, "the kernel publishes no "
                               "firmware event log (%s)", path)
                   : set_error_errno(CLI_ERR_IO, "opening %s", path);
    buf = malloc(LOG_MAX);
    if (buf == NULL) {
        (void)close(fd);
        return set_error(CLI_ERR_NOMEM, "the event log");
    }
    for (;;) {
        ssize_t k = read(fd, buf + n, LOG_MAX - n);

        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0) {
            (void)close(fd);
            free(buf);
            return set_error_errno(CLI_ERR_IO, "reading %s", path);
        }
        if (k == 0)
            break;
        n += (size_t)k;
        if (n == LOG_MAX) {
            (void)close(fd);
            free(buf);
            return set_error(CLI_ERR_RANGE, "%s is larger than 4 MiB", path);
        }
    }
    (void)close(fd);
    *out = buf;
    *len = n;
    return CLI_OK;
}

typedef struct {
    const uint8_t *p;
    size_t         n;
} mem_file;

static int mem_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const mem_file *f = ctx;

    if (off > f->n || len > f->n - off)
        return -1;
    memcpy(buf, f->p + off, (size_t)len);
    return 0;
}

/* The station image's Authenticode digest in the log's bank: what shim and
 * the firmware record for an image they load. */
static cli_status station_digest(stn_esp *e, int sha1, uint8_t out[32])
{
    static uint8_t scratch[65536];
    uint8_t *img = malloc(EFI_MAX);
    ac_layout l;
    mem_file f;
    size_t got = 0;
    cli_status rc;

    if (img == NULL)
        return set_error(CLI_ERR_NOMEM, "the station image");
    rc = esp_read(e, STATION_EFI, img, EFI_MAX, &got);
    if (rc != CLI_OK) {
        free(img);
        return set_error(CLI_ERR_NOTFOUND, "\\EFI\\falcon\\" STATION_EFI
                         " could not be read off the ESP");
    }
    f.p = img;
    f.n = got;
    if (ac_layout_read(img, got < 65536u ? got : 65536u, got, &l) != AC_OK ||
        ac_hash_with(&l, sha1 ? hash_sha1() : hash_sha256(), mem_read, &f,
                     scratch, sizeof scratch, out) != AC_OK) {
        free(img);
        return set_error(CLI_ERR_INVALID, "\\EFI\\falcon\\" STATION_EFI
                         " is not a PE image");
    }
    free(img);
    return CLI_OK;
}

/* The PCR values the station runs with, from the log, checked. */
static cli_status station_pcrs(stn_esp *e, const char *log_file, int live,
                               tpm_version ver, uint32_t mask,
                               uint8_t vals[][32])
{
    static evlog_replay rep;
    static evlog_replay all;
    uint8_t digest[32];
    uint16_t alg = ver == TPM_VER_20 ? EVLOG_ALG_SHA256 : EVLOG_ALG_SHA1;
    size_t dl = ver == TPM_VER_20 ? 32u : 20u;
    uint8_t *log = NULL;
    size_t len = 0;
    unsigned i;
    size_t n = 0;
    int rc;
    cli_status st;

    st = station_digest(e, ver == TPM_VER_12, digest);
    if (st != CLI_OK)
        return st;
    st = read_log(log_file, &log, &len);
    if (st != CLI_OK)
        return st;
    rc = evlog_replay_run(log, len, alg, 4, digest, &rep);
    if (rc == EVLOG_OK)
        rc = evlog_replay_run(log, len, alg, 0, NULL, &all);
    free(log);
    if (rc == EVLOG_ERR_BANK)
        return set_error(CLI_ERR_INVALID, "the event log has no %s bank",
                         ver == TPM_VER_20 ? "SHA-256" : "SHA-1");
    if (rc != EVLOG_OK)
        return set_error(CLI_ERR_INVALID, "the event log does not parse");
    if (!rep.stopped)
        return set_error(CLI_ERR_INVALID, "this system was not started by "
                         "the station on %s: the firmware's event log has no "
                         "measurement of its \\EFI\\falcon\\" STATION_EFI
                         ". Enroll from a system the station started",
                         e->s.disks[0].d.path);
    for (i = 0; i < STATIONINI_TOTP_PCRS; i++) {
        if (((mask >> i) & 1u) == 0)
            continue;
        if (live) {
            uint8_t now[32];

            rc = ver == TPM_VER_20 ? tpm2_pcr_read(&g_dev, i, now)
                                   : tpm12_pcr_read(&g_dev, i, now);
            if (rc != TPM_OK)
                return tpm_fail(rc, "reading the PCRs");
            if (memcmp(now, all.pcr[i], dl) != 0)
                return set_error(CLI_ERR_INVALID, "the event log does not "
                                 "account for PCR %u as the TPM holds it; "
                                 "enrolling would seal to values the station "
                                 "never sees", i);
        }
        memset(vals[n], 0, 32);
        memcpy(vals[n], rep.pcr[i], dl);
        n++;
    }
    return CLI_OK;
}

/* ---- the commands ---- */

static const cli_opt enroll_opts[] = {
    { "pcrs", CLI_OPT_VALUE, 0, "<list>",
      "PCRs to seal to, 0 to 15 (default " TOTP_PCRS_DEFAULT ")" },
    { "owner-auth-file", CLI_OPT_VALUE, 0, "<file>",
      "the TPM owner's authorisation (default: none on 2.0, the "
      "well-known secret on 1.2)" },
    { "label", CLI_OPT_VALUE, 0, "<name>",
      "the authenticator's account name (default: this host's name)" },
    { "tpm-socket", CLI_OPT_VALUE, 0, "<path>",
      "TEST ONLY: swtpm's socket instead of /dev/tpmrm0 or /dev/tpm0" },
    { "event-log", CLI_OPT_VALUE, 0, "<file>",
      "TEST ONLY: the firmware event log from a file instead of securityfs" }
};

static const cli_opt remove_opts[] = {
    { "owner-auth-file", CLI_OPT_VALUE, 0, "<file>",
      "the TPM owner's authorisation (default: none on 2.0, the "
      "well-known secret on 1.2)" },
    { "tpm-socket", CLI_OPT_VALUE, 0, "<path>",
      "TEST ONLY: swtpm's socket instead of /dev/tpmrm0 or /dev/tpm0" }
};

/* Deletes an index, as the owner; one that is not there is not an error. */
static void undefine(tpm_version ver, const owner_auth *o, uint32_t index)
{
    if (ver == TPM_VER_20)
        (void)tpm2_nv_undefine(&g_dev, o->v, o->n, index);
    else
        (void)tpm12_nv_undefine(&g_dev, o->v12, index);
}

/* Which of the two well-known SRK secrets this TPM has: the station opens
 * its transport session with the SRK's secret, and a 1.2 TPM owned with a
 * password of zero bytes holds their SHA-1 rather than the bytes. A read of
 * the new index through the session tells: the session's own authorisation
 * is checked first (TPM_AUTHFAIL), the PCRs after it (the read then fails
 * as a PCR mismatch here, which is expected, since this system's PCRs are
 * not the station's). */
static int srk_secret(const tpm12_pubkey *srk, uint32_t index,
                      stationini_totp *t)
{
    static const uint8_t zeros[20] = { 0 };
    uint8_t auth[20];
    uint8_t out[TOTP_SEED_LEN];
    int k;

    for (k = 0; k < 2; k++) {
        int rc;

        if (k == 0)
            memset(auth, 0, sizeof auth);
        else
            sha1(zeros, sizeof zeros, auth);
        rc = tpm12_seed_read(&g_dev, index, auth, srk, out, sizeof out);
        crypto_wipe(out, sizeof out);
        if (rc == TPM_OK || rc == TPM_ERR_PCR) {
            t->srk_sha1 = (uint8_t)k;
            return TPM_OK;
        }
        if (rc != TPM_ERR_RC || g_dev.rc != 0x1u)
            return rc;
    }
    return TPM_ERR_KEY;
}

static cli_status write_ini(stn_esp *e, const stationini_totp *t)
{
    static char out[INI_MAX];
    char why[STATIONINI_WHY_MAX];
    unsigned line = 0;
    size_t n = 0;
    int rc;

    rc = totpc_ini_update(e->have_ini ? e->ini : NULL,
                          e->have_ini ? e->ini_len : 0, t, out, sizeof out,
                          &n, &line, why);
    if (rc == TOTPC_ERR_INI)
        return set_error(CLI_ERR_INVALID, "%s line %u: %s", STATIONINI_FILE,
                         line, why);
    if (rc != TOTPC_OK)
        return set_error(CLI_ERR_RANGE, "%s would be larger than 16 KB",
                         STATIONINI_FILE);
    return esp_write_ini(e, out, n);
}

static void show_secret(const char *account, const uint8_t *seed)
{
    char secret[64];
    char uri[TOTP_URI_MAX];

    if (totpc_base32(seed, TOTP_SEED_LEN, secret, sizeof secret) != TOTPC_OK ||
        totpc_uri(account, seed, TOTP_SEED_LEN, uri, sizeof uri) != TOTPC_OK)
        return;
    cli_log_blank();
    log_info("Add this to an authenticator now; it is not shown again.");
    log_info("  account: %s:%s", TOTP_ISSUER, account);
    log_info("  secret:  %s", secret);
    log_info("  uri:     %s", uri);
    if (proc_exists("qrencode")) {
        (void)proc_runl("qrencode", "-t", "ANSIUTF8", uri, PROC_END);
    } else {
        log_info("qrencode is not installed, so there is no QR code: enter "
                 "the secret by hand.");
    }
    crypto_wipe(secret, sizeof secret);
    crypto_wipe(uri, sizeof uri);
}

static cli_status enroll(const cli_args *a, int reseal)
{
    static stn_esp e;
    static uint8_t vals[STATIONINI_TOTP_PCRS][32];
    owner_auth o;
    stationini_totp t;
    tpm_version ver = TPM_VER_NONE;
    const char *sock = cli_value(a, "tpm-socket", NULL);
    const char *logf = cli_value(a, "event-log", NULL);
    const char *label = cli_value(a, "label", NULL);
    uint8_t seed[TOTP_SEED_LEN];
    char host[64];
    uint32_t mask = 0;
    uint32_t index;
    int rc;
    cli_status st;

    if (totpc_parse_pcrs(cli_value(a, "pcrs", TOTP_PCRS_DEFAULT), &mask) !=
        TOTPC_OK)
        return set_error(CLI_ERR_USAGE, "--pcrs: PCR numbers 0 to 15, each "
                         "once, comma-separated");
    if (sock != NULL && logf == NULL)
        return set_error(CLI_ERR_USAGE, "--tpm-socket needs --event-log");
    if (label == NULL) {
        if (gethostname(host, sizeof host) != 0)
            (void)snprintf(host, sizeof host, "station");
        host[sizeof host - 1u] = '\0';
        label = host;
    }
    if (strlen(TOTP_ISSUER ":") + strlen(label) >= STATIONINI_LABEL_MAX ||
        strpbrk(label, ";#") != NULL)
        return set_error(CLI_ERR_USAGE, "--label: up to %u characters, no ; "
                         "or #", (unsigned)(STATIONINI_LABEL_MAX - 16u));

    st = esp_open(&e, a->pos[0]);
    if (st != CLI_OK)
        return st;
    if (!reseal && e.parsed.totp.present)
        return set_error(CLI_ERR_EXISTS, "the station on %s already has a "
                         "TOTP enrolled; `station totp reseal` replaces it",
                         a->pos[0]);
    if (reseal && !e.parsed.totp.present)
        return set_error(CLI_ERR_NOTFOUND, "the station on %s has no TOTP "
                         "enrolled; `station totp enroll` makes one",
                         a->pos[0]);
    st = owner_read(cli_value(a, "owner-auth-file", NULL), &o);
    if (st != CLI_OK)
        return st;

    st = tpm_open(sock, &ver);
    if (st == CLI_OK)
        st = station_pcrs(&e, logf, sock == NULL, ver, mask, vals);
    if (st != CLI_OK) {
        tpm_close();
        crypto_wipe(&o, sizeof o);
        return st;
    }
    index = ver == TPM_VER_20 ? TPM2_TOTP_INDEX : TPM12_TOTP_INDEX;
    log_info("TPM %s, index 0x%08x, PCRs %s", ver == TPM_VER_20 ? "2.0" : "1.2",
             index, cli_value(a, "pcrs", TOTP_PCRS_DEFAULT));

    if (getrandom_rng(NULL, seed, sizeof seed) != 0) {
        tpm_close();
        crypto_wipe(&o, sizeof o);
        return set_error(CLI_ERR_IO, "no randomness for the seed");
    }
    memset(&t, 0, sizeof t);
    t.present = 1;
    t.index = index;
    t.pcrs = mask;
    (void)snprintf(t.label, sizeof t.label, "%s:%s", TOTP_ISSUER, label);

    /* An index left by an earlier enrollment, or by a reseal's old one, is
     * replaced: its policy is fixed at definition. */
    if (e.parsed.totp.present)
        undefine(ver, &o, e.parsed.totp.index);
    undefine(ver, &o, index);
    if (ver == TPM_VER_20) {
        t.tpm = 20;
        rc = tpm2_seed_enroll_values(&g_dev, o.v, o.n, index, mask,
                                     (const uint8_t (*)[32])vals, seed,
                                     sizeof seed, t.key);
        t.key_len = TPM2_NAME_LEN;
    } else {
        static tpm12_pubkey srk;
        uint8_t v12[STATIONINI_TOTP_PCRS][20];
        unsigned i;

        for (i = 0; i < STATIONINI_TOTP_PCRS; i++)
            memcpy(v12[i], vals[i], 20);
        t.tpm = 12;
        rc = tpm12_seed_enroll_values(&g_dev, o.v12, index, mask,
                                      (const uint8_t (*)[20])v12, seed,
                                      sizeof seed, &srk);
        if (rc == TPM_OK &&
            (srk.exp_len != 3u || srk.exp[0] != 1u || srk.exp[1] != 0u ||
             srk.exp[2] != 1u || srk.mod_len < 128u ||
             srk.mod_len > STATIONINI_TOTP_KEY_MAX)) {
            undefine(ver, &o, index);
            tpm_close();
            crypto_wipe(seed, sizeof seed);
            crypto_wipe(&o, sizeof o);
            return set_error(CLI_ERR_INVALID, "this TPM's SRK is not a 1024 "
                             "to 2048-bit RSA key with exponent 65537");
        }
        memcpy(t.key, srk.mod, srk.mod_len);
        t.key_len = (uint16_t)srk.mod_len;
        if (rc == TPM_OK)
            rc = srk_secret(&srk, index, &t);
    }
    if (rc != TPM_OK) {
        st = tpm_fail(rc, "sealing the seed");
        tpm_close();
        crypto_wipe(seed, sizeof seed);
        crypto_wipe(&o, sizeof o);
        return st;
    }

    st = write_ini(&e, &t);
    if (st != CLI_OK) {
        undefine(ver, &o, index);
        tpm_close();
        crypto_wipe(seed, sizeof seed);
        crypto_wipe(&o, sizeof o);
        return st;
    }
    tpm_close();
    crypto_wipe(&o, sizeof o);
    log_info("sealed; \\EFI\\falcon\\%s on %s partition %u has the [totp] "
             "section", STATIONINI_FILE, e.s.disks[0].d.path, e.number);
    show_secret(label, seed);
    crypto_wipe(seed, sizeof seed);
    return CLI_OK;
}

static cli_status enroll_run(const cli_args *a)
{
    return enroll(a, 0);
}

static cli_status reseal_run(const cli_args *a)
{
    return enroll(a, 1);
}

static cli_status remove_run(const cli_args *a)
{
    static stn_esp e;
    owner_auth o;
    tpm_version ver = TPM_VER_NONE;
    tpm_version want;
    cli_status st;

    st = esp_open(&e, a->pos[0]);
    if (st != CLI_OK)
        return st;
    if (!e.parsed.totp.present)
        return set_error(CLI_ERR_NOTFOUND, "the station on %s has no TOTP "
                         "enrolled", a->pos[0]);
    st = owner_read(cli_value(a, "owner-auth-file", NULL), &o);
    if (st == CLI_OK)
        st = tpm_open(cli_value(a, "tpm-socket", NULL), &ver);
    if (st != CLI_OK) {
        tpm_close();
        crypto_wipe(&o, sizeof o);
        return st;
    }
    want = e.parsed.totp.tpm == 20 ? TPM_VER_20 : TPM_VER_12;
    if (ver != want) {
        tpm_close();
        crypto_wipe(&o, sizeof o);
        return set_error(CLI_ERR_INVALID, "STATION.INI names a TPM %s and "
                         "this machine's is a %s",
                         want == TPM_VER_20 ? "2.0" : "1.2",
                         ver == TPM_VER_20 ? "2.0" : "1.2");
    }
    {
        int rc = ver == TPM_VER_20
                     ? tpm2_nv_undefine(&g_dev, o.v, o.n,
                                        e.parsed.totp.index)
                     : tpm12_nv_undefine(&g_dev, o.v12, e.parsed.totp.index);

        if (rc != TPM_OK)
            log_warn("the TPM did not delete index 0x%08x (code 0x%08x); the "
                     "section is removed anyway", e.parsed.totp.index,
                     g_dev.rc);
    }
    tpm_close();
    crypto_wipe(&o, sizeof o);
    st = write_ini(&e, NULL);
    if (st != CLI_OK)
        return st;
    log_info("removed: the station shows no TOTP code from its next boot");
    return CLI_OK;
}

static const cli_cmd totp_enroll = {
    "enroll",
    "seal a new TOTP seed to this boot and record it in STATION.INI",
    "<disk>",
    enroll_opts,
    sizeof enroll_opts / sizeof enroll_opts[0],
    1, 1,
    "Run on the station's machine, from a system the station started.\n"
    "Seals a new 20-byte seed into the TPM (NV index 0x01800FA1 on 2.0,\n"
    "0x20000FA1 on 1.2), readable only while the PCRs hold what they hold\n"
    "when the station runs, which are read from the firmware's event log up\n"
    "to the station's own measurement. Writes STATION.INI's [totp] and\n"
    "prints the secret once, with a QR code when qrencode is installed.\n"
    "--owner-auth-file: the file's bytes, a trailing newline dropped; on 1.2\n"
    "exactly 20 bytes is the secret itself, anything else a passphrase whose\n"
    "SHA-1 it is.\n",
    enroll_run
};

static const cli_cmd totp_reseal = {
    "reseal",
    "enroll again after the firmware, shim or the station changed",
    "<disk>",
    enroll_opts,
    sizeof enroll_opts / sizeof enroll_opts[0],
    1, 1,
    "enroll over an existing enrollment: a new seed, sealed to this boot.\n"
    "The authenticator's old entry stops matching; replace it.\n",
    reseal_run
};

static const cli_cmd totp_remove = {
    "remove",
    "delete the seed from the TPM and [totp] from STATION.INI",
    "<disk>",
    remove_opts,
    sizeof remove_opts / sizeof remove_opts[0],
    1, 1,
    "Deletes the NV index as the owner and removes the section.\n",
    remove_run
};

static const cli_cmd *const totp_cmds[] = {
    &totp_enroll,
    &totp_reseal,
    &totp_remove
};

static cli_status totp_placeholder(const cli_args *a)
{
    (void)a;
    return set_error(CLI_ERR_USAGE, "station totp enroll, reseal or remove?");
}

static const cli_cmd totp_topic = {
    "totp",
    "the UEFI station's TOTP check: enroll, reseal, remove",
    "enroll|reseal|remove <disk> ...",
    NULL,
    0,
    0, 0,
    "  station totp enroll <disk> [--pcrs 0,2,4,7] [--owner-auth-file F]\n"
    "  station totp reseal <disk> [--pcrs ...] [--owner-auth-file F]\n"
    "  station totp remove <disk> [--owner-auth-file F]\n"
    "\n"
    "`falcon-boot-mkstick station totp <command> --help` for each.\n",
    totp_placeholder
};

cli_status stn_totp_main(int argc, char **argv)
{
    int i;
    int k;

    for (i = 0; i < argc; i++) {
        if (argv[i][0] != '-')
            break;
    }
    if (i >= argc) {
        int help = 0;

        for (k = 0; k < argc; k++) {
            if (strcmp(argv[k], "--help") == 0 || strcmp(argv[k], "-h") == 0)
                help = 1;
        }
        cli_help_set_topic(&totp_topic);
        if (help) {
            cli_help_print();
            return CLI_OK;
        }
        return set_error(CLI_ERR_USAGE,
                         "station totp enroll, reseal or remove?");
    }
    return cli_dispatch(totp_cmds, sizeof totp_cmds / sizeof totp_cmds[0],
                        "falcon-boot-mkstick station totp", argc, argv);
}
