/* falcon-boot mkstick: `station totp enroll|reseal|remove`, the UEFI
 * station's TOTP check (docs/DESIGN.md Section 12.15).
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
 * the station has loaded a UKI and moved PCR 4 (and shim PCR 7) on. The
 * station reads them itself before it loads anything and saves them
 * (tpm/evsave.h); enrollment seals to that record. On a TPM 2.0 the record
 * is checked against the firmware's event log where the log accounts for
 * the PCRs as they are now: replayed up to the event that measured the
 * station's own image (falcon-core/src/tpm/evlog.h), it must give what the
 * station read. A log that does not account for them, from a firmware that
 * extends PCRs it does not log, is set aside, and the PCRs nothing extends
 * after the station's menu show the record to be of this boot. A station
 * older than the record saves none, and the replay alone then gives the
 * values, as long as the log accounts for the PCRs. Finding the station's
 * measurement in the log is also the proof that this system was started
 * by the station on the named disk; without it enrollment refuses.
 *
 * Check runs the station's read from here, which the TPM refuses on the
 * PCRs when everything before them (index, key, secret) is right.
 *
 * Reseal is enrollment again over an existing one: the seed cannot be read
 * back from this system (its PCRs are not the station's), so a new one is
 * made and the authenticator's entry is replaced. Remove deletes the index
 * and the section.
 *
 * A TPM 1.2 behind edk2 on coreboot has two logs, and Linux reads only
 * coreboot's. There the station saves edk2's at every boot, with the PCRs
 * it found (tpm/evsave.h), and enrollment takes the PCRs from that record
 * instead: checked to be what a replay of the saved log gives, alone or
 * after the one Linux reads (totpc_merge12), and to hold the station's own
 * measurement. The live check then covers only the PCRs nothing extends
 * after the station's menu (TOTPC_STABLE_PCRS), which are also what show
 * the record and Linux's log to be of this boot.
 *
 * Three options are for the TPM rig only (falcon-efi/tools/run-tpm-rig.sh):
 * --tpm-socket talks to swtpm's socket instead of a device, --event-log
 * reads the log from a file (the one the rig's probe saved in the VM), and
 * --saved-log the station's record from a file (the probe saves that too).
 * With --tpm-socket the TPM is not the one that booted, so the log is not
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
#include <mkstick/shimname.h>
#include <mkstick/station.h>
#include <mkstick/totpcore.h>

#include <crypto/crypto.h>
#include <crypto/hash.h>
#include <crypto/sha1.h>
#include <fs/fs.h>
#include <ident/ident.h>
#include <media/stationini.h>
#include <tpm/evlog.h>
#include <tpm/evsave.h>
#include <tpm/tpm.h>
#include <tpm/tpm12.h>
#include <tpm/tpm2.h>
#include <trust/authenticode.h>

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
    /* The loader's name in \EFI\falcon (stn_esp_loader_name). */
    char      loader[SHIM_NEXT_STAGE_MAX];
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
    rc = stn_esp_loader_name(e->s.disks[0].d.path, e->off, e->len, e->loader,
                             sizeof e->loader);
    if (rc != CLI_OK)
        return rc;
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

cli_status stn_image_digest(const uint8_t *img, size_t n, int sha1,
                            uint8_t out[32])
{
    static uint8_t scratch[65536];
    ac_layout l;
    mem_file f;

    f.p = img;
    f.n = n;
    if (ac_layout_read(img, n < 65536u ? n : 65536u, n, &l) != AC_OK ||
        ac_hash_with(&l, sha1 ? hash_sha1() : hash_sha256(), mem_read, &f,
                     scratch, sizeof scratch, out) != AC_OK)
        return set_error(CLI_ERR_INVALID, "not a PE image");
    return CLI_OK;
}

/* The station image's Authenticode digest in the log's bank: what shim and
 * the firmware record for an image they load. */
static cli_status station_digest(stn_esp *e, int sha1, uint8_t out[32])
{
    uint8_t *img = malloc(EFI_MAX);
    size_t got = 0;
    cli_status rc;

    if (img == NULL)
        return set_error(CLI_ERR_NOMEM, "the station image");
    rc = esp_read(e, e->loader, img, EFI_MAX, &got);
    if (rc != CLI_OK) {
        free(img);
        return set_error(CLI_ERR_NOTFOUND, "\\EFI\\falcon\\%s could not "
                         "be read off the ESP", e->loader);
    }
    rc = stn_image_digest(img, got, sha1, out);
    free(img);
    if (rc != CLI_OK)
        return set_error(CLI_ERR_INVALID, "\\EFI\\falcon\\%s is not a "
                         "PE image", e->loader);
    return CLI_OK;
}

/* The station's saved record: from efivarfs (its attribute bytes dropped),
 * or a file. CLI_ERR_NOTFOUND when there is none: a machine booted by an
 * older station, or whose firmware took no record. */
static cli_status read_saved(const char *file, uint8_t **out, size_t *len)
{
    const char *path = file != NULL ? file : TOTPC_EVLOG_EFIVAR;
    size_t skip = file != NULL ? 0u : 4u;
    void *data = NULL;
    size_t n = 0;
    cli_status rc;

    if (file == NULL && access(path, F_OK) != 0)
        return CLI_ERR_NOTFOUND;
    rc = util_read_file(path, &data, &n, EVSAVE_BODY + LOG_MAX);
    if (rc != CLI_OK)
        return rc;
    if (n < skip) {
        free(data);
        return set_error(CLI_ERR_INVALID, "%s is too short", path);
    }
    memmove(data, (uint8_t *)data + skip, n - skip);
    *out = data;
    *len = n - skip;
    return CLI_OK;
}

/* The PCR values at the station's menu on a TPM 1.2, from the record it
 * saved (see the top of this file), checked. */
static cli_status station_pcrs_saved(stn_esp *e, const char *log_file,
                                     const uint8_t *rec, size_t rec_len,
                                     int live, uint32_t mask,
                                     const uint8_t *old1,
                                     uint8_t vals[][32])
{
    static uint8_t pcr[EVSAVE_PCRS][EVSAVE_DIGEST_LEN];
    uint8_t digest[32];
    uint8_t *log = NULL;
    size_t len = 0;
    unsigned from = 0;
    unsigned bad = 0;
    unsigned i;
    size_t n = 0;
    int rc;
    cli_status st;

    st = station_digest(e, 1, digest);
    if (st != CLI_OK)
        return st;
    /* Linux's log is the second log when there is one; a machine without
     * one may still have everything in the saved log. */
    st = read_log(log_file, &log, &len);
    if (st != CLI_OK && !(st == CLI_ERR_NOTFOUND && log_file == NULL))
        return st;
    /* With old1, this boot ran the loader the ESP had before mkstick
     * update: its measurement is found by old1 and taken as the new
     * loader's, whose values the station will read after the reboot. */
    rc = totpc_merge12_as(log, len, rec, rec_len,
                          old1 != NULL ? old1 : digest,
                          old1 != NULL ? digest : NULL, pcr, &from, &bad);
    free(log);
    switch (rc) {
    case TOTPC_OK:
        break;
    case TOTPC_ERR_STATION:
        return set_error(CLI_ERR_INVALID, "this system was not started by "
                         "the station on %s: the event log it saved has no "
                         "measurement of its \\EFI\\falcon\\%s. Enroll "
                         "from a system the station started",
                         e->s.disks[0].d.path, e->loader);
    case TOTPC_ERR_PCR:
        return set_error(CLI_ERR_INVALID, "the event logs do not account for "
                         "PCR %u as the station read it; enrolling would seal "
                         "to values the station never sees", bad);
    default:
        return set_error(CLI_ERR_INVALID, "the event log the station saved "
                         "does not parse");
    }
    log_info("PCRs from the event log the station saved%s",
             from == TOTPC_FROM_BOTH ? ", after the kernel's" : "");
    for (i = 0; i < STATIONINI_TOTP_PCRS; i++) {
        uint8_t now[32];

        if (live && ((TOTPC_STABLE_PCRS >> i) & 1u) != 0) {
            rc = tpm12_pcr_read(&g_dev, i, now);
            if (rc != TPM_OK)
                return tpm_fail(rc, "reading the PCRs");
            if (memcmp(now, pcr[i], EVSAVE_DIGEST_LEN) != 0)
                return set_error(CLI_ERR_INVALID, "PCR %u is not what the "
                                 "station read: the event log it saved is "
                                 "not of this boot", i);
        }
        if (((mask >> i) & 1u) == 0)
            continue;
        memset(vals[n], 0, 32);
        memcpy(vals[n], pcr[i], EVSAVE_DIGEST_LEN);
        n++;
    }
    return CLI_OK;
}

/* The PCR values at the station's menu on a TPM 2.0, from the record it
 * saved, checked against the firmware's log where that log accounts for
 * the PCRs (totpc_station20). With old256, mkstick update's reseal: the
 * new loader's values are a prediction from the log, which needs a whole
 * one, so the record is checked with the old loader's digest and the
 * prediction left to the one-log path (*predict). */
static cli_status station_pcrs_saved20(stn_esp *e, const char *log_file,
                                       const uint8_t *rec, size_t rec_len,
                                       int live, uint32_t mask,
                                       const uint8_t *old256,
                                       uint8_t vals[][32], int *predict)
{
    static uint8_t now[16][32];
    static uint8_t pcr[16][32];
    uint8_t digest[32];
    uint8_t *log = NULL;
    size_t len = 0;
    unsigned bad = 0;
    unsigned i;
    size_t n = 0;
    int whole = 0;
    int rc;
    cli_status st;

    *predict = 0;
    st = station_digest(e, 0, digest);
    if (st != CLI_OK)
        return st;
    st = read_log(log_file, &log, &len);
    if (st != CLI_OK)
        return st;
    if (live) {
        for (i = 0; i < 16u; i++) {
            rc = tpm2_pcr_read(&g_dev, i, now[i]);
            if (rc != TPM_OK) {
                free(log);
                return tpm_fail(rc, "reading the PCRs");
            }
        }
    }
    rc = totpc_station20(rec, rec_len, log, len,
                         old256 != NULL ? old256 : digest,
                         live ? (const uint8_t (*)[32])now : NULL, mask, pcr,
                         &whole, &bad);
    free(log);
    switch (rc) {
    case TOTPC_OK:
        break;
    case TOTPC_ERR_STATION:
        return set_error(CLI_ERR_INVALID, "this system was not started by "
                         "the station on %s: the firmware's event log has no "
                         "measurement of its \\EFI\\falcon\\%s. Enroll "
                         "from a system the station started",
                         e->s.disks[0].d.path, e->loader);
    case TOTPC_ERR_BOOT:
        return set_error(CLI_ERR_INVALID, "PCR %u is not what the station "
                         "read: the PCRs it saved are not of this boot", bad);
    case TOTPC_ERR_PCR:
        return set_error(CLI_ERR_INVALID, "the event log and the PCRs the "
                         "station read disagree on PCR %u; enrolling would "
                         "seal to values the station never sees", bad);
    default:
        return set_error(CLI_ERR_INVALID, "the PCRs the station saved or the "
                         "event log do not parse");
    }
    if (old256 != NULL) {
        if (!whole)
            return set_error(CLI_ERR_INVALID, "this firmware extends PCRs it "
                             "does not log, so the new station's PCRs cannot "
                             "be predicted; run `station totp reseal` from a "
                             "boot of it");
        *predict = 1;
        return CLI_OK;
    }
    if (whole)
        log_info("PCRs the station read, matching the event log");
    else
        log_info("PCRs the station read: this firmware extends PCRs it does "
                 "not log, so the event log is set aside");
    for (i = 0; i < STATIONINI_TOTP_PCRS; i++) {
        if (((mask >> i) & 1u) == 0)
            continue;
        memcpy(vals[n], pcr[i], 32);
        n++;
    }
    return CLI_OK;
}

/* Says what the station wrote in FalconEvLogStatus when it saved no
 * record: where its read stopped, or nothing when it is a station older
 * than the status line. Only from efivarfs: the rig's --saved-log has
 * none. */
static void say_no_record(const char *saved_file)
{
    char text[EVSAVE_STATUS_MAX + 1u];
    void *data = NULL;
    size_t n = 0;

    if (saved_file != NULL || access(TOTPC_EVSTATUS_EFIVAR, F_OK) != 0 ||
        util_read_file(TOTPC_EVSTATUS_EFIVAR, &data, &n,
                       EVSAVE_STATUS_MAX + 4u) != CLI_OK) {
        cli_error_clear();
        log_info("the station saved no PCRs; a station older than the "
                 "record saves none, so the event log alone gives them");
        return;
    }
    if (n > 4u) {
        size_t i;

        n -= 4u;
        memcpy(text, (uint8_t *)data + 4, n);
        for (i = 0; i < n; i++) {
            if (text[i] < 0x20 || text[i] > 0x7E)
                text[i] = '?';
        }
        text[n] = '\0';
        log_warn("the station saved no PCRs: %s", text);
    }
    free(data);
}

/* The PCR values the station runs with, from the log, checked. */
static cli_status station_pcrs(stn_esp *e, const char *log_file,
                               const char *saved_file, int live,
                               tpm_version ver, uint32_t mask,
                               const uint8_t *old1, const uint8_t *old256,
                               uint8_t vals[][32])
{
    const uint8_t *old = ver == TPM_VER_20 ? old256 : old1;
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

    if (ver == TPM_VER_12) {
        uint8_t *rec = NULL;
        size_t rec_len = 0;

        st = read_saved(saved_file, &rec, &rec_len);
        if (st == CLI_OK) {
            st = station_pcrs_saved(e, log_file, rec, rec_len, live, mask,
                                    old1, vals);
            free(rec);
            return st;
        }
        if (st != CLI_ERR_NOTFOUND)
            return st;
        say_no_record(saved_file);
    } else {
        uint8_t *rec = NULL;
        size_t rec_len = 0;
        int predict = 0;

        /* A station older than the record saves none; the log alone then
         * stands, as before. */
        st = read_saved(saved_file, &rec, &rec_len);
        if (st == CLI_OK) {
            st = station_pcrs_saved20(e, log_file, rec, rec_len, live, mask,
                                      old256, vals, &predict);
            free(rec);
            if (st != CLI_OK || !predict)
                return st;
        } else if (st != CLI_ERR_NOTFOUND) {
            return st;
        } else {
            say_no_record(saved_file);
        }
    }
    st = station_digest(e, ver == TPM_VER_12, digest);
    if (st != CLI_OK)
        return st;
    st = read_log(log_file, &log, &len);
    if (st != CLI_OK)
        return st;
    /* With old, as in station_pcrs_saved: the replay stops at the old
     * loader's measurement and takes it as the new one's. */
    rc = evlog_replay_init(alg, &rep);
    if (rc == EVLOG_OK) {
        if (old != NULL)
            evlog_replay_substitute(&rep, 4, old, digest);
        rc = evlog_replay_more(log, len, 4, old != NULL ? old : digest, &rep);
    }
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
                         "measurement of its \\EFI\\falcon\\%s. Enroll "
                         "from a system the station started",
                         e->s.disks[0].d.path, e->loader);
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
      "TEST ONLY: the firmware event log from a file instead of securityfs" },
    { "saved-log", CLI_OPT_VALUE, 0, "<file>",
      "TEST ONLY: the station's saved record (PCRs, and the 1.2 event log) "
      "from a file instead of efivarfs" }
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
        totpc_uri(account, seed, TOTP_SEED_LEN, uri, sizeof uri) != TOTPC_OK) {
        log_error("the secret could not be shown; `station totp reseal` "
                  "makes a new one");
        return;
    }
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

/* What an enrollment is asked for: from the command line (enroll, reseal),
 * or from mkstick update (stn_totp_reseal_update). */
typedef struct {
    const char    *disk;
    const char    *pcrs;       /* NULL: TOTP_PCRS_DEFAULT */
    const char    *label;      /* NULL: the host name */
    const char    *owner;      /* --owner-auth-file, or NULL */
    const char    *sock;
    const char    *logf;
    const char    *savedf;
    int            reseal;
    /* Keep the enrolled [totp]'s PCRs and label rather than pcrs and
     * label: an update's reseal is the same enrollment, sealed again. */
    int            keep;
    /* The loader this boot ran, when it is no longer the one on the ESP
     * (station_pcrs): SHA-1 and SHA-256 Authenticode digests, or NULL. */
    const uint8_t *old1;
    const uint8_t *old256;
} enroll_in;

static cli_status enroll(const enroll_in *in)
{
    static stn_esp e;
    static uint8_t vals[STATIONINI_TOTP_PCRS][32];
    owner_auth o;
    stationini_totp t;
    tpm_version ver = TPM_VER_NONE;
    const char *sock = in->sock;
    const char *logf = in->logf;
    const char *savedf = in->savedf;
    const char *label = in->label;
    const char *pcrs = in->pcrs != NULL ? in->pcrs : TOTP_PCRS_DEFAULT;
    uint8_t seed[TOTP_SEED_LEN];
    char host[64];
    uint32_t mask = 0;
    uint32_t index;
    int rc;
    cli_status st;

    if (!in->keep && totpc_parse_pcrs(pcrs, &mask) != TOTPC_OK)
        return set_error(CLI_ERR_USAGE, "--pcrs: PCR numbers 0 to 15, each "
                         "once, comma-separated");
    if (sock != NULL && logf == NULL && savedf == NULL)
        return set_error(CLI_ERR_USAGE, "--tpm-socket needs --event-log or "
                         "--saved-log");
    if (label == NULL && !in->keep) {
        if (gethostname(host, sizeof host) != 0)
            (void)snprintf(host, sizeof host, "station");
        host[sizeof host - 1u] = '\0';
        label = host;
    }
    if (!in->keep &&
        (strlen(TOTP_ISSUER ":") + strlen(label) >= STATIONINI_LABEL_MAX ||
         strpbrk(label, ";#") != NULL))
        return set_error(CLI_ERR_USAGE, "--label: up to %u characters, no ; "
                         "or #", (unsigned)(STATIONINI_LABEL_MAX - 16u));

    st = esp_open(&e, in->disk);
    if (st != CLI_OK)
        return st;
    if (!in->reseal && e.parsed.totp.present)
        return set_error(CLI_ERR_EXISTS, "the station on %s already has a "
                         "TOTP enrolled; `station totp reseal` replaces it",
                         in->disk);
    if (in->reseal && !e.parsed.totp.present)
        return set_error(CLI_ERR_NOTFOUND, "the station on %s has no TOTP "
                         "enrolled; `station totp enroll` makes one",
                         in->disk);
    if (in->keep)
        mask = e.parsed.totp.pcrs;
    st = owner_read(in->owner, &o);
    if (st != CLI_OK)
        return st;

    st = tpm_open(sock, &ver);
    if (st == CLI_OK)
        st = station_pcrs(&e, logf, savedf, sock == NULL, ver, mask,
                          in->old1, in->old256, vals);
    if (st != CLI_OK) {
        tpm_close();
        crypto_wipe(&o, sizeof o);
        return st;
    }
    index = ver == TPM_VER_20 ? TPM2_TOTP_INDEX : TPM12_TOTP_INDEX;
    log_info("TPM %s, index 0x%08x, PCRs %s", ver == TPM_VER_20 ? "2.0" : "1.2",
             index, in->keep ? "as enrolled" : pcrs);

    if (getrandom_rng(NULL, seed, sizeof seed) != 0) {
        tpm_close();
        crypto_wipe(&o, sizeof o);
        return set_error(CLI_ERR_IO, "no randomness for the seed");
    }
    memset(&t, 0, sizeof t);
    t.present = 1;
    t.index = index;
    t.pcrs = mask;
    if (in->keep)
        (void)snprintf(t.label, sizeof t.label, "%s", e.parsed.totp.label);
    else
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
    /* A kept label is the issuer's and the account together; the URI wants
     * the account alone. */
    if (in->keep) {
        const char *colon = strchr(t.label, ':');

        label = colon != NULL ? colon + 1 : t.label;
    }
    show_secret(label, seed);
    crypto_wipe(seed, sizeof seed);
    return CLI_OK;
}

static void enroll_args(const cli_args *a, int reseal, enroll_in *in)
{
    memset(in, 0, sizeof *in);
    in->disk = a->pos[0];
    in->pcrs = cli_value(a, "pcrs", NULL);
    in->label = cli_value(a, "label", NULL);
    in->owner = cli_value(a, "owner-auth-file", NULL);
    in->sock = cli_value(a, "tpm-socket", NULL);
    in->logf = cli_value(a, "event-log", NULL);
    in->savedf = cli_value(a, "saved-log", NULL);
    in->reseal = reseal;
}

cli_status stn_totp_reseal_update(const stn_totp_update *u)
{
    enroll_in in;

    memset(&in, 0, sizeof in);
    in.disk = u->disk;
    in.owner = u->owner_auth_file;
    in.sock = u->tpm_socket;
    in.logf = u->event_log;
    in.savedf = u->saved_log;
    in.reseal = 1;
    in.keep = 1;
    in.old1 = u->old_sha1;
    in.old256 = u->old_sha256;
    return enroll(&in);
}

static cli_status enroll_run(const cli_args *a)
{
    enroll_in in;

    enroll_args(a, 0, &in);
    return enroll(&in);
}

static cli_status reseal_run(const cli_args *a)
{
    enroll_in in;

    enroll_args(a, 1, &in);
    return enroll(&in);
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

/* ---- check ---- */

static const cli_opt check_opts[] = {
    { "tpm-socket", CLI_OPT_VALUE, 0, "<path>",
      "TEST ONLY: swtpm's socket instead of /dev/tpmrm0 or /dev/tpm0" }
};

/* The station's own read, from here: the same session, key and secret it
 * uses at boot. This system's PCRs are not the station's, so the TPM
 * refusing on the PCRs is the answer that shows everything before them
 * (the index, the pinned key, the SRK's secret on 1.2) is right; any other
 * refusal names the step. */
static cli_status check_run(const cli_args *a)
{
    static stn_esp e;
    const stationini_totp *t;
    tpm_version ver = TPM_VER_NONE;
    tpm_version want;
    uint8_t seed[TOTP_SEED_LEN];
    const char *step;
    int rc;
    cli_status st;

    st = esp_open(&e, a->pos[0]);
    if (st != CLI_OK)
        return st;
    t = &e.parsed.totp;
    if (!t->present)
        return set_error(CLI_ERR_NOTFOUND, "the station on %s has no TOTP "
                         "enrolled", a->pos[0]);
    st = tpm_open(cli_value(a, "tpm-socket", NULL), &ver);
    if (st != CLI_OK) {
        tpm_close();
        return st;
    }
    want = t->tpm == 20 ? TPM_VER_20 : TPM_VER_12;
    if (ver != want) {
        tpm_close();
        return set_error(CLI_ERR_INVALID, "STATION.INI names a TPM %s and "
                         "this machine's is a %s",
                         want == TPM_VER_20 ? "2.0" : "1.2",
                         ver == TPM_VER_20 ? "2.0" : "1.2");
    }
    if (ver == TPM_VER_20) {
        rc = tpm2_seed_read(&g_dev, t->index, t->pcrs, t->key, seed,
                            sizeof seed);
    } else {
        static const uint8_t zeros[20] = { 0 };
        tpm12_pubkey pk;
        uint8_t auth[20];

        memset(&pk, 0, sizeof pk);
        memcpy(pk.mod, t->key, t->key_len);
        pk.mod_len = t->key_len;
        pk.exp[0] = 0x01;
        pk.exp[1] = 0x00;
        pk.exp[2] = 0x01;
        pk.exp_len = 3;
        if (t->srk_sha1)
            sha1(zeros, sizeof zeros, auth);
        else
            memset(auth, 0, sizeof auth);
        rc = tpm12_seed_read(&g_dev, t->index, auth, &pk, seed, sizeof seed);
    }
    crypto_wipe(seed, sizeof seed);
    step = ver == TPM_VER_12 ? tpm12_ord_name(g_dev.ord) : NULL;
    tpm_close();

    switch (rc) {
    case TPM_OK:
        log_info("the TPM released the seed to this system: its PCRs now "
                 "are the enrolled ones");
        return CLI_OK;
    case TPM_ERR_PCR:
        log_info("the TPM refused on the PCRs, as it does outside the "
                 "station: the index, the storage key%s recorded in "
                 "STATION.INI are this TPM's",
                 ver == TPM_VER_12 ? " and its secret" : "");
        return CLI_OK;
    case TPM_ERR_RC:
        return set_error(CLI_ERR_IO, "the TPM refused: code 0x%08x%s%s",
                         g_dev.rc, step != NULL ? " in " : "",
                         step != NULL ? step : "");
    case TPM_ERR_AUTH:
        return set_error(CLI_ERR_INVALID, "the TPM's answer did not verify");
    case TPM_ERR_KEY:
        return set_error(CLI_ERR_INVALID, "the TPM's storage key is not the "
                         "one STATION.INI pins");
    default:
        return tpm_fail(rc, "reading the seed");
    }
}

static const cli_cmd totp_check = {
    "check",
    "run the station's read of the seed from here, and say where it stops",
    "<disk>",
    check_opts,
    sizeof check_opts / sizeof check_opts[0],
    1, 1,
    "The station's own read with STATION.INI's [totp]: its index, pinned\n"
    "key and, on 1.2, the storage key's secret. From a running system the\n"
    "TPM refuses on the PCRs, which says everything before them is right;\n"
    "any other refusal is named with the step that got it. Changes nothing.\n",
    check_run
};

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
    &totp_remove,
    &totp_check
};

static cli_status totp_placeholder(const cli_args *a)
{
    (void)a;
    return set_error(CLI_ERR_USAGE, "station totp enroll, reseal, remove or "
                     "check?");
}

static const cli_cmd totp_topic = {
    "totp",
    "the UEFI station's TOTP check: enroll, reseal, remove, check",
    "enroll|reseal|remove|check <disk> ...",
    NULL,
    0,
    0, 0,
    "  station totp enroll <disk> [--pcrs 0,2,4,7] [--owner-auth-file F]\n"
    "  station totp reseal <disk> [--pcrs ...] [--owner-auth-file F]\n"
    "  station totp remove <disk> [--owner-auth-file F]\n"
    "  station totp check <disk>\n"
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
