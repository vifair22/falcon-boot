/* falcon-boot-sign: blessing a bare EFI payload (docs/DESIGN.md Section 7.2).
 * See payload.h for the contract and for why no manifest record is written.
 *
 * Two things here are worth the reader's attention before the code.
 *
 * THE SIGNING DISCIPLINE, AND WHY IT IS THE OPPOSITE OF sign-uki's. sbsign
 * appends a certificate and never replaces one, so a tool that signs the same
 * file twice stacks certificates on it. The repo's answer to that, in the
 * user's own sign-uki script, is to strip every signature first so exactly one
 * remains: ours. That is right for an artifact WE produce - our UKI wraps,
 * where we are the only legitimate signer and the old signature is a previous
 * version of our own.
 *
 * It is WRONG here, and the distinction is worth learning once. A payload is
 * somebody else's binary: memtest's loader ships signed by the Microsoft
 * UEFI CA, which is what makes it boot on nearly every machine in the world
 * today. Stripping that to add ours would make it boot on this user's laptop
 * (whose db carries only their key) and stop it booting everywhere else - a
 * strict regression, performed silently, on a file the user did not write. So
 * this path ADDS trust rather than replacing it: every signature already there
 * is preserved, ours is appended beside it, and the result is checked to
 * confirm both. Blessing does not stack our own certificate either, because a
 * payload that already carries ours is recognised and left alone.
 *
 * Strip when we own the artifact; append when we are adding trust to someone
 * else's.
 *
 * HOW THE .img IS OPENED. The payload inside an image lives in a FAT
 * filesystem, and it has to be REPLACED by a longer file, which is a
 * filesystem write. The tool does not implement one: the kernel's own vfat
 * driver does the work, reached through a loop device configured for one
 * partition's byte range. The alternative, mtools, is one more package that
 * has to exist both on the user's host and inside the on-stick appliance's
 * initramfs, where the only binaries are the five staged there on purpose; the
 * syscalls below need nothing staged at all. The cost is that this path needs
 * root, which the appliance already is and a host user must ask for, and the
 * message says so rather than failing obscurely.
 *
 * That is a mount of media the user did not produce, which src/cli/sign's ISO
 * reader deliberately avoids. The difference is that reading an ISO has a
 * root-free answer (list it with a helper) and writing into a FAT image does
 * not, and that the user has explicitly asked for this image to be modified.
 * The mount is as narrow as the flags allow: read-only for the survey pass,
 * nosuid/nodev/noexec always, and torn down before the next volume is looked
 * at.
 */

#include <cli/sign/sign.h>
#include <cli/sign/payload.h>

#include <family/family.h>
#include <part/part.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/loop.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

/* Where a removable medium's default loader lives, by the UEFI spec's own
 * rule (3.5.1.1) and the one path the loader's chainload ever asks for. FAT
 * resolves it case-insensitively, which is the same rule the firmware's File
 * Protocol follows, so what is signed here is what the firmware will find. */
#define PAYLOAD_MEMBER "EFI/BOOT/BOOTX64.EFI"

/* Volumes inside one image that are looked at. A disk image with more than a
 * handful of partitions is not a payload image; the cap is a bound on this
 * tool's work, and a table longer than it is reported rather than ignored. */
#define MAX_VOLS 16u

/* sbverify --list prints a few lines per signature. */
#define LIST_MAX (256u << 10)

/* Content fingerprints read the head of a small file (family.c asks for 1 KB
 * of a loader entry); nothing legitimate in a probe needs more. */
#define PROBE_READ_MAX (64u << 10)

/* ------------------------------------------------------------ shape */

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int ext_is(const char *path, const char *ext)
{
    const char *base = sgn_base_name(path);
    size_t bl = strlen(base);
    size_t el = strlen(ext);
    size_t i;

    if (bl <= el)
        return 0;
    for (i = 0; i < el; i++) {
        if (lower_ascii(base[bl - el + i]) != ext[i])
            return 0;
    }
    return 1;
}

sgn_payload_shape sgn_payload_shape_of(const char *path)
{
    if (path == NULL)
        return SGN_PAYLOAD_NONE;
    if (ext_is(path, ".efi"))
        return SGN_PAYLOAD_EFI;
    if (ext_is(path, ".img"))
        return SGN_PAYLOAD_IMG;
    return SGN_PAYLOAD_NONE;
}

/* ------------------------------------------------------------ signatures */

/* How many certificates the image carries. SGN_ERR_FORMAT when sbverify
 * cannot read it at all, which for this tool means the file is not a PE and
 * so is not a payload. */
static int sig_count(const char *path, int *out)
{
    char *argv[4];
    char *text = NULL;
    size_t len = 0;
    size_t i = 0;
    int n = 0;
    int rc;

    *out = 0;
    argv[0] = sgn_arg("sbverify");
    argv[1] = sgn_arg("--list");
    argv[2] = sgn_arg(path);
    argv[3] = NULL;
    rc = sgn_run_capture(argv, &text, &len, LIST_MAX);
    if (rc != SGN_OK) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "'%s' is not a PE image sbverify can read, so it is not "
                      "an EFI payload", path);
        return SGN_ERR_FORMAT;
    }

    /* One "signature <n>" line per certificate, at the start of a line; the
     * indented lines under it describe the one above and must not be counted
     * as another. */
    while (i < len) {
        size_t start = i;

        while (i < len && text[i] != '\n')
            i++;
        if (i > start && (i - start) > 10u &&
            memcmp(text + start, "signature ", 10u) == 0)
            n++;
        if (i < len)
            i++;
    }
    free(text);
    *out = n;
    return SGN_OK;
}

/* ---- "is this signed with OUR certificate?" ----
 *
 * NOT `sbverify --cert`, and the reason is measured rather than stylistic.
 * sbverify 0.9.5 answers "Signature verification OK" for PassMark's
 * Microsoft-signed MemTest86 loader against this repo's throwaway MOK
 * certificate, and against a second unrelated certificate generated for the
 * experiment (2026-09-13, on the very binary this path exists for). It
 * discriminates correctly for a self-signed image with a one-certificate
 * chain, which is why the wart is easy to miss: every image the test suite
 * would naturally use is that shape. An "already signed with your key" that
 * answers yes for anything vendor-signed would silently skip the one file the
 * user asked to have signed.
 *
 * So the question is asked of the bytes: does the image's certificate table
 * contain the exact DER encoding of the certificate we are about to sign
 * with? That is what an image signed with this key carries, it needs no
 * tooling at all, and it cannot be talked into a false yes by a chain it does
 * not understand. A wrong answer here is not a security failure but it is not
 * free either: since signing APPENDS, a false no would add a second copy of
 * our own certificate on every run. This check is what keeps that from
 * happening, which is why it is exact bytes rather than a verdict.
 */

/* Largest certificate table read back. Authenticode tables are kilobytes; a
 * megabyte is three orders of magnitude of headroom and keeps a corrupt size
 * field from making the tool allocate a gigabyte. */
#define CERT_TABLE_MAX (1u << 20)

#define OPT_MAGIC_PE32   0x10bu
#define OPT_MAGIC_PE32P  0x20bu
#define DIR_SECURITY     4u

/* Byte offset of the certificate table's entry inside the data directory
 * array (eight bytes per entry). size_t rather than the unsigned int the
 * arithmetic would default to, so the offsets below are computed in the type
 * they are used in. */
#define SEC_ENTRY_OFF    ((size_t)DIR_SECURITY * 8u)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* The DER of the first certificate in a PEM file. Base64 between the two
 * markers and nothing else: this is not an X.509 parser and does not need to
 * be, because the whole use of the result is to look for it verbatim inside
 * another file. */
int sgn_payload_cert_der(const char *path, uint8_t **out, size_t *out_len)
{
    char *text = NULL;
    size_t len = 0;
    uint8_t *der;
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    const char *begin = "-----BEGIN CERTIFICATE-----";
    const char *end = "-----END CERTIFICATE-----";
    char *p;
    char *stop;
    int rc;

    *out = NULL;
    *out_len = 0;
    rc = sgn_read_file(path, &text, &len, LIST_MAX);
    if (rc != SGN_OK)
        return rc;

    p = strstr(text, begin);
    stop = (p != NULL) ? strstr(p, end) : NULL;
    if (p == NULL || stop == NULL) {
        free(text);
        sgn_set_error(SGN_ERR_FORMAT,
                      "'%s' holds no PEM certificate", path);
        return SGN_ERR_FORMAT;
    }
    p += strlen(begin);

    der = malloc((size_t)(stop - p) + 4u);
    if (der == NULL) {
        free(text);
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return SGN_ERR_IO;
    }
    while (p < stop) {
        int v = b64_val(*p);

        if (v >= 0) {
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der[n++] = (uint8_t)((acc >> bits) & 0xFFu);
            }
        }
        p++;
    }
    free(text);
    if (n == 0) {
        free(der);
        sgn_set_error(SGN_ERR_FORMAT, "'%s' holds an empty certificate", path);
        return SGN_ERR_FORMAT;
    }
    *out = der;
    *out_len = n;
    return SGN_OK;
}

/* The image's certificate table (PE data directory 4), or SGN_OK with a NULL
 * buffer when it has none. The directory's first field is a FILE offset for
 * this one directory rather than an RVA (PE/COFF spec 5.7), which is what
 * makes reading it a seek rather than a section walk. */
int sgn_payload_cert_table(const char *path, uint8_t **out, size_t *out_len)
{
    uint8_t head[1024];
    uint8_t *tab;
    FILE *f;
    size_t got;
    size_t pe;
    size_t opt;
    size_t nrva_off;
    size_t dir_off;
    uint32_t off;
    uint32_t size;

    *out = NULL;
    *out_len = 0;
    f = fopen(path, "rb");
    if (f == NULL) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", path);
        return SGN_ERR_IO;
    }
    got = fread(head, 1, sizeof(head), f);

    if (got < 0x40u || head[0] != 'M' || head[1] != 'Z')
        goto not_pe;
    pe = (size_t)le32(head + 0x3c);
    if (pe > got || got - pe < 26u || memcmp(head + pe, "PE\0\0", 4) != 0)
        goto not_pe;
    opt = pe + 24u;
    if (got - opt < 2u)
        goto not_pe;
    if (le16(head + opt) == OPT_MAGIC_PE32P) {
        nrva_off = 108u;
        dir_off = 112u;
    } else if (le16(head + opt) == OPT_MAGIC_PE32) {
        nrva_off = 92u;
        dir_off = 96u;
    } else {
        goto not_pe;
    }
    if (got - opt < nrva_off + 4u || le32(head + opt + nrva_off) <= DIR_SECURITY)
        goto none;
    if (got - opt < dir_off + SEC_ENTRY_OFF + 8u)
        goto not_pe;
    off = le32(head + opt + dir_off + SEC_ENTRY_OFF);
    size = le32(head + opt + dir_off + SEC_ENTRY_OFF + 4u);
    if (off == 0 || size == 0)
        goto none;
    if (size > CERT_TABLE_MAX) {
        (void)fclose(f);
        sgn_set_error(SGN_ERR_FORMAT,
                      "'%s' claims a %u-byte certificate table", path,
                      (unsigned)size);
        return SGN_ERR_FORMAT;
    }

    tab = malloc(size);
    if (tab == NULL) {
        (void)fclose(f);
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return SGN_ERR_IO;
    }
    if (fseek(f, (long)off, SEEK_SET) != 0 ||
        fread(tab, 1, size, f) != (size_t)size) {
        free(tab);
        (void)fclose(f);
        sgn_set_error(SGN_ERR_FORMAT,
                      "'%s' has a certificate table its own headers point "
                      "past the end of the file", path);
        return SGN_ERR_FORMAT;
    }
    (void)fclose(f);
    *out = tab;
    *out_len = size;
    return SGN_OK;

not_pe:
    (void)fclose(f);
    sgn_set_error(SGN_ERR_FORMAT, "'%s' is not a PE image", path);
    return SGN_ERR_FORMAT;
none:
    (void)fclose(f);
    return SGN_OK;
}

static int bytes_contain(const uint8_t *hay, size_t hn, const uint8_t *needle,
                         size_t nn)
{
    size_t i;

    if (nn == 0 || nn > hn)
        return 0;
    for (i = 0; i + nn <= hn; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nn) == 0)
            return 1;
    }
    return 0;
}

/* Nonzero when the image's certificate table carries this exact certificate.
 * Errors answer "no": they are all "the image cannot be read as a signed PE",
 * and the caller's next step (sign it) is the right answer to that. */
int sgn_payload_signed_with(const char *path, const uint8_t *der,
                            size_t der_len)
{
    uint8_t *tab = NULL;
    size_t tab_len = 0;
    int yes;

    if (sgn_payload_cert_table(path, &tab, &tab_len) != SGN_OK ||
        tab == NULL)
        return 0;
    yes = bytes_contain(tab, tab_len, der, der_len);
    free(tab);
    return yes;
}

static int copy_file(const char *src, const char *dst, mode_t mode)
{
    char buf[64u << 10];
    int in = open(src, O_RDONLY | O_CLOEXEC);
    int out;

    if (in < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", src);
        return SGN_ERR_IO;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "create '%s'", dst);
        (void)close(in);
        return SGN_ERR_IO;
    }
    for (;;) {
        ssize_t got = read(in, buf, sizeof(buf));
        ssize_t done = 0;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            sgn_set_error_errno(SGN_ERR_IO, "read '%s'", src);
            goto fail;
        }
        if (got == 0)
            break;
        while (done < got) {
            ssize_t put = write(out, buf + done, (size_t)(got - done));

            if (put < 0) {
                if (errno == EINTR)
                    continue;
                sgn_set_error_errno(SGN_ERR_IO, "write '%s'", dst);
                goto fail;
            }
            done += put;
        }
    }
    (void)close(in);
    if (close(out) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "close '%s'", dst);
        (void)unlink(dst);
        return SGN_ERR_IO;
    }
    return SGN_OK;

fail:
    (void)close(in);
    (void)close(out);
    (void)unlink(dst);
    return SGN_ERR_IO;
}

/* Signs one PE in place, ADDING our signature to whatever it already carries.
 * See payload.h for the discipline; the append-rather-than-strip rule is the
 * part worth reading twice.
 *
 * *already is set when the payload was left untouched because our certificate
 * is already in it. */
static int sign_in_place(const char *path, const char *key, const char *cert,
                         const uint8_t *der, size_t der_len, int *already)
{
    char tmp[PATH_MAX];
    char out[PATH_MAX];
    struct stat st;
    uint8_t *before = NULL;
    uint8_t *after = NULL;
    size_t before_len = 0;
    size_t after_len = 0;
    int had = 0;
    int have = 0;
    int rc;

    *already = 0;
    if (stat(path, &st) != 0) {
        sgn_set_error_errno(SGN_ERR_IO, "stat '%s'", path);
        return SGN_ERR_IO;
    }
    rc = sig_count(path, &had);
    if (rc != SGN_OK)
        return rc;
    /* Our certificate is already in it: nothing to do, and this is the whole
     * of the no-stacking rule. Blessing the same payload twice cannot add a
     * second copy of our own signature, because the second run stops here. */
    if (sgn_payload_signed_with(path, der, der_len)) {
        *already = 1;
        return SGN_OK;
    }
    /* Kept so the result can be checked against it: what was there before must
     * still be there afterwards. */
    rc = sgn_payload_cert_table(path, &before, &before_len);
    if (rc != SGN_OK)
        return rc;

    /* The temporaries sit beside the payload so the final rename stays within
     * one filesystem, which is what makes the replacement atomic: the payload
     * is either the original or the fully signed copy, never a half-written
     * file. On a FAT image that filesystem is the image's own. */
    if (snprintf(tmp, sizeof(tmp), "%s.falcon-in.%ld", path,
                 (long)getpid()) >= (int)sizeof(tmp) ||
        snprintf(out, sizeof(out), "%s.falcon-signed.%ld", path,
                 (long)getpid()) >= (int)sizeof(out)) {
        sgn_set_error(SGN_ERR_NOSPACE,
                      "the payload's path is too long to stage beside: '%s'",
                      path);
        free(before);
        return SGN_ERR_NOSPACE;
    }

    rc = copy_file(path, tmp, st.st_mode & 07777u);
    if (rc != SGN_OK) {
        free(before);
        return rc;
    }

    {
        char *argv[9];

        argv[0] = sgn_arg("sbsign");
        argv[1] = sgn_arg("--key");
        argv[2] = sgn_arg(key);
        argv[3] = sgn_arg("--cert");
        argv[4] = sgn_arg(cert);
        argv[5] = sgn_arg("--output");
        argv[6] = out;
        argv[7] = tmp;
        argv[8] = NULL;
        rc = sgn_run(argv);
        if (rc != SGN_OK)
            goto done;
    }

    /* Verified before it replaces a bootable image, never after, and the
     * three checks are three different ways of being wrong:
     *
     *   one more signature   sbsign appended rather than replaced
     *   ours is in it        it appended OUR certificate
     *   the old table is
     *   still in there       nothing the vendor signed was dropped
     *
     * The third is the one a naive test misses. "Our certificate verifies"
     * passes just as happily on a strip-and-replace, and a strip-and-replace
     * is precisely the regression this path must not commit: it would make
     * the payload boot here and stop it booting on every machine that trusts
     * the vendor and not us. */
    rc = sig_count(out, &have);
    if (rc != SGN_OK)
        goto done;
    if (have != had + 1) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "signing '%s' took it from %d signature(s) to %d; it "
                      "should have gained exactly one", path, had, have);
        rc = SGN_ERR_FORMAT;
        goto done;
    }
    if (!sgn_payload_signed_with(out, der, der_len)) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "the signed copy of '%s' does not carry the certificate "
                      "from '%s'", path, cert);
        rc = SGN_ERR_FORMAT;
        goto done;
    }
    rc = sgn_payload_cert_table(out, &after, &after_len);
    if (rc != SGN_OK)
        goto done;
    if (before != NULL &&
        !bytes_contain(after, after_len, before, before_len)) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "signing '%s' did not preserve the signature(s) it "
                      "already carried; refusing to install a payload that "
                      "would stop booting where it boots today", path);
        rc = SGN_ERR_FORMAT;
        goto done;
    }

    /* The payload keeps the mode it had; sbsign's output is 0644 whatever the
     * original was, and a loader that lost its executable bit on the way
     * through this tool would be a surprise nobody asked for. */
    if (chmod(out, st.st_mode & 07777u) != 0) {
        sgn_set_error_errno(SGN_ERR_IO, "chmod '%s'", out);
        rc = SGN_ERR_IO;
        goto done;
    }
    if (rename(out, path) != 0) {
        sgn_set_error_errno(SGN_ERR_IO, "replace '%s'", path);
        rc = SGN_ERR_IO;
        goto done;
    }
    out[0] = '\0';   /* renamed: there is nothing left to unlink */

done:
    free(before);
    free(after);
    (void)unlink(tmp);
    if (out[0] != '\0')
        (void)unlink(out);
    return rc;
}

/* ------------------------------------------------------ FAT volumes */

typedef struct {
    int  img_fd;
    int  loop_fd;
    char dev[32];
    char mnt[PATH_MAX];
    int  mounted;
} fatvol;

static void fatvol_init(fatvol *v)
{
    v->img_fd = -1;
    v->loop_fd = -1;
    v->dev[0] = '\0';
    v->mnt[0] = '\0';
    v->mounted = 0;
}

/* Every teardown step is attempted even if an earlier one failed: a loop
 * device left attached to the user's image outlives this process, and there
 * is no later chance to take it back. */
static void fatvol_close(fatvol *v)
{
    if (v->mounted) {
        /* umount is the flush: it writes the filesystem back before it
         * returns, which is what makes the image on disk complete by the time
         * the loop device is detached below. Retried on EBUSY because that is
         * the transient one (something still holds a reference); every other
         * errno is reported rather than spun on. */
        int tries = 0;

        while (umount(v->mnt) != 0) {
            if (errno != EBUSY || ++tries >= 3) {
                sgn_log(SGN_LOG_WARN, "could not unmount %s: %s", v->mnt,
                        strerror(errno));
                break;
            }
        }
        v->mounted = 0;
    }
    if (v->mnt[0] != '\0') {
        (void)rmdir(v->mnt);
        v->mnt[0] = '\0';
    }
    if (v->loop_fd >= 0) {
        if (ioctl(v->loop_fd, LOOP_CLR_FD, 0) != 0)
            sgn_log(SGN_LOG_WARN, "could not detach %s: %s", v->dev,
                    strerror(errno));
        (void)close(v->loop_fd);
        v->loop_fd = -1;
    }
    if (v->img_fd >= 0) {
        (void)close(v->img_fd);
        v->img_fd = -1;
    }
    v->dev[0] = '\0';
}

/* Attaches one byte range of the image to a free loop device and mounts it as
 * vfat under work. Returns SGN_ERR_FORMAT when the range holds no filesystem
 * the kernel will mount, which is the ordinary answer for a partition that is
 * not FAT and is not an error worth stopping the run for. */
static int fatvol_open(fatvol *v, const char *img, uint64_t off, uint64_t len,
                       const char *work, int rw)
{
    struct loop_config cfg;
    int ctl;
    long idx;

    fatvol_init(v);

    v->img_fd = open(img, rw ? (O_RDWR | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC));
    if (v->img_fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", img);
        return SGN_ERR_IO;
    }

    ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (ctl < 0) {
        sgn_set_error_errno(SGN_ERR_IO,
                            "open /dev/loop-control (blessing a .img needs "
                            "the loop driver and root; run this as root, or "
                            "let the on-stick appliance do it)");
        fatvol_close(v);
        return SGN_ERR_IO;
    }
    idx = ioctl(ctl, LOOP_CTL_GET_FREE);
    (void)close(ctl);
    if (idx < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "no free loop device");
        fatvol_close(v);
        return SGN_ERR_IO;
    }
    if (snprintf(v->dev, sizeof(v->dev), "/dev/loop%ld", idx) >=
        (int)sizeof(v->dev)) {
        sgn_set_error(SGN_ERR_NOSPACE, "loop device index %ld", idx);
        fatvol_close(v);
        return SGN_ERR_NOSPACE;
    }
    v->loop_fd = open(v->dev, O_RDWR | O_CLOEXEC);
    if (v->loop_fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", v->dev);
        fatvol_close(v);
        return SGN_ERR_IO;
    }

    /* One configure call rather than SET_FD plus SET_STATUS64: the two-step
     * form leaves a window in which the device is attached with the wrong
     * offset, and a partition mounted at offset zero is the whole image. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = (uint32_t)v->img_fd;
    cfg.info.lo_offset = off;
    cfg.info.lo_sizelimit = len;
    cfg.info.lo_flags = rw ? 0u : (uint32_t)LO_FLAGS_READ_ONLY;
    if (ioctl(v->loop_fd, LOOP_CONFIGURE, &cfg) != 0) {
        sgn_set_error_errno(SGN_ERR_IO, "configure %s over '%s'", v->dev, img);
        (void)close(v->loop_fd);
        v->loop_fd = -1;           /* never attached: nothing to clear */
        fatvol_close(v);
        return SGN_ERR_IO;
    }

    if (snprintf(v->mnt, sizeof(v->mnt), "%s/falcon-sign-mnt-%ld-%ld", work,
                 (long)getpid(), idx) >= (int)sizeof(v->mnt)) {
        sgn_set_error(SGN_ERR_NOSPACE, "the mount point path is too long");
        v->mnt[0] = '\0';
        fatvol_close(v);
        return SGN_ERR_NOSPACE;
    }
    if (mkdir(v->mnt, 0700) != 0 && errno != EEXIST) {
        sgn_set_error_errno(SGN_ERR_IO, "mkdir '%s'", v->mnt);
        v->mnt[0] = '\0';
        fatvol_close(v);
        return SGN_ERR_IO;
    }

    /* Narrow on purpose: this is media the user did not produce, so nothing
     * on it may be a device node, a setuid binary or something to execute. */
    {
        unsigned long flags = (unsigned long)(MS_NOATIME | MS_NOSUID |
                                              MS_NODEV | MS_NOEXEC);

        if (!rw)
            flags |= (unsigned long)MS_RDONLY;
        if (mount(v->dev, v->mnt, "vfat", flags, NULL) != 0) {
            int err = errno;

            fatvol_close(v);
            if (err == EACCES || err == EPERM) {
                sgn_set_error(SGN_ERR_IO,
                              "mounting '%s' needs root (blessing a .img "
                              "writes into the filesystem inside it); run "
                              "this as root, or let the on-stick appliance "
                              "do it", img);
                return SGN_ERR_IO;
            }
            return SGN_ERR_FORMAT;   /* not FAT: the caller moves on */
        }
        v->mounted = 1;
    }
    return SGN_OK;
}

/* ---- the family probe, over a mounted volume ---- */

static int probe_path(char *buf, size_t cap, const char *root,
                      const char *path)
{
    while (*path == '/')
        path++;
    return snprintf(buf, cap, "%s/%s", root, path) < (int)cap;
}

static int probe_exists(void *ctx, const char *path)
{
    char full[PATH_MAX];
    struct stat st;

    if (!probe_path(full, sizeof(full), (const char *)ctx, path))
        return 0;
    return stat(full, &st) == 0;
}

static int probe_read(void *ctx, const char *path, uint64_t off, void *buf,
                      size_t len, size_t *got)
{
    char full[PATH_MAX];
    int fd;
    ssize_t n;

    if (got != NULL)
        *got = 0;
    if (len > PROBE_READ_MAX)
        return 1;
    if (!probe_path(full, sizeof(full), (const char *)ctx, path))
        return 1;
    fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 1;
    n = pread(fd, buf, len, (off_t)off);
    (void)close(fd);
    if (n < 0)
        return 1;
    if (got != NULL)
        *got = (size_t)n;
    return 0;
}

/* family.c unchanged, over a directory tree instead of an ISO listing: the
 * module reaches a filesystem only through these two callbacks precisely so
 * the same fingerprints answer for every medium (family.h). */
static const family_desc *volume_family(const char *root)
{
    family_probe fp;

    fp.exists = probe_exists;
    fp.read = probe_read;
    fp.ctx = (void *)(uintptr_t)root;
    return family_detect(&fp);
}

/* ---- the image's volumes ---- */

typedef struct {
    uint64_t off;
    uint64_t len;
    uint32_t number;    /* GPT index, or 0 for a whole-image volume */
} img_vol;

static int img_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    int fd = *(const int *)ctx;
    ssize_t n = pread(fd, buf, (size_t)len, (off_t)off);

    return (n < 0 || (uint64_t)n != len) ? 1 : 0;
}

/* The byte ranges inside the image that could hold a filesystem: one per GPT
 * partition, or the whole file when there is no partition table at all (a
 * superfloppy, which the loader boots the same way the firmware's FAT driver
 * binds to one). *count receives how many were written. */
static int img_volumes(const char *path, img_vol *out, size_t cap,
                       size_t *count)
{
    gpt_part parts[MAX_VOLS];
    gpt_table tab;
    uint64_t size = 0;
    uint32_t i;
    int fd;
    int rc;

    *count = 0;
    rc = sgn_file_size(path, &size);
    if (rc != SGN_OK)
        return rc;
    if (size == 0) {
        sgn_set_error(SGN_ERR_FORMAT, "'%s' is empty", path);
        return SGN_ERR_FORMAT;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "open '%s'", path);
        return SGN_ERR_IO;
    }
    tab.parts = parts;
    tab.cap = MAX_VOLS;
    tab.count = 0;
    tab.truncated = 0;
    rc = gpt_parse(&tab, img_read, &fd, 512u);
    (void)close(fd);

    if (rc != GPT_OK) {
        /* No usable table is not a failure: the image is then one volume,
         * which is what a plain ESP image or a mkfs'd FAT file is. */
        sgn_log(SGN_LOG_DEBUG, "%s has no usable GPT (%d); treating the whole "
                "image as one volume", path, rc);
        if (cap == 0)
            return SGN_ERR_NOSPACE;
        out[0].off = 0;
        out[0].len = size;
        out[0].number = 0;
        *count = 1;
        return SGN_OK;
    }
    if (tab.truncated)
        sgn_log(SGN_LOG_WARN,
                "'%s' has more than %u partitions; only the first %u are "
                "looked at", path, (unsigned)MAX_VOLS, (unsigned)MAX_VOLS);

    for (i = 0; i < tab.count && *count < cap; i++) {
        uint64_t start = parts[i].start_lba * 512u;
        uint64_t end = (parts[i].end_lba + 1u) * 512u;

        if (end <= start || end > size)
            continue;
        out[*count].off = start;
        out[*count].len = end - start;
        out[*count].number = parts[i].number;
        (*count)++;
    }
    if (*count == 0) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "'%s' has a partition table but no partition that fits "
                      "inside the file", path);
        return SGN_ERR_FORMAT;
    }
    return SGN_OK;
}

/* ------------------------------------------------------------ the bless */

/* What one volume of an image turned out to hold. */
typedef struct {
    img_vol   vol;
    family_id fam;
    int       has_payload;
} vol_survey;

/* Reads every volume without writing to any of them: which families they look
 * like, and which of them carry a payload at the removable-media path. This
 * runs before a single byte is written, because what the warning says has to
 * be true of the image in front of the user. */
static int survey_image(const char *img, const char *work, vol_survey *out,
                        size_t cap, size_t *count, int *payloads)
{
    img_vol vols[MAX_VOLS];
    size_t n = 0;
    size_t i;
    int rc;

    *count = 0;
    *payloads = 0;
    rc = img_volumes(img, vols, cap < MAX_VOLS ? cap : MAX_VOLS, &n);
    if (rc != SGN_OK)
        return rc;

    for (i = 0; i < n; i++) {
        char member[PATH_MAX];
        fatvol v;

        rc = fatvol_open(&v, img, vols[i].off, vols[i].len, work, 0);
        if (rc == SGN_ERR_FORMAT)
            continue;           /* not a filesystem we can read */
        if (rc != SGN_OK)
            return rc;

        out[*count].vol = vols[i];
        out[*count].fam = volume_family(v.mnt)->id;
        out[*count].has_payload = 0;
        if (snprintf(member, sizeof(member), "%s/" PAYLOAD_MEMBER, v.mnt) <
                (int)sizeof(member) &&
            sgn_file_exists(member)) {
            out[*count].has_payload = 1;
            (*payloads)++;
        }
        (*count)++;
        fatvol_close(&v);
    }
    return SGN_OK;
}

/* The statement the whole of Section 7.2 turns on. Printed to stdout, before
 * anything is opened for writing, naming the file and every payload inside it
 * that is about to be written to. "we modified your .img" is not something
 * anyone can audit; a partition number and a path inside it is. */
static void announce_head(const char *path)
{
    (void)printf("THIS BLESS MODIFIES YOUR FILE: %s\n", path);
}

static void announce_tail(void)
{
    (void)printf(
"  A bare EFI payload boots as itself, so it is signed in place rather than\n"
"  wrapped. Signing appends a certificate: the file grows and is no longer\n"
"  byte-identical to what its vendor published. Signatures it already carries\n"
"  are kept.\n");
    /* Said before it is done, and that has to hold when stdout is a pipe as
     * well as when it is a terminal: without this the helpers' own stderr
     * reaches the log first and the statement arrives after the act. */
    (void)fflush(stdout);
}

static int bless_efi(const sgn_payload_opts *o, const uint8_t *der,
                    size_t der_len)
{
    int already = 0;
    int rc;

    announce_head(o->path);
    (void)printf("  the payload IS this file, and it is signed in place.\n");
    announce_tail();
    if (o->dry_run) {
        (void)printf("would sign %s\n", o->path);
        return SGN_OK;
    }

    rc = sign_in_place(o->path, o->key, o->cert, der, der_len, &already);
    if (rc != SGN_OK)
        return rc;
    if (already)
        (void)printf("already signed with this key, left alone: %s\n",
                     o->path);
    else
        (void)printf("signed %s\n", o->path);
    return SGN_OK;
}

/* Why this family's image must not be blessed as a bare payload, or NULL when
 * it may be.
 *
 * A family whose boot runs anything after the payload is refused rather than
 * half-blessed: signing its loader would report success and leave the next
 * stage for the platform to refuse, which is exactly the failure a user cannot
 * diagnose from what the tool said. The permissive answer is for the two that
 * genuinely end at the payload: memtest, which runs no second stage at all,
 * and an image nothing recognised, where the user has looked at it and this
 * tool has not. */
const char *sgn_payload_refusal(family_id fam)
{
    switch (fam) {
    case FAMILY_UNKNOWN:
    case FAMILY_MEMTEST:
        return NULL;
    case FAMILY_WINDOWS:
        return "Windows media: its boot chain is Microsoft-signed the whole "
               "way down, and replacing that signature would break it. Use "
               "`prepare' instead";
    default:
        return "media whose boot runs a kernel this would leave unsigned, so "
               "signing the loader would report success and still be refused. "
               "Bless the .iso of the same release instead";
    }
}

static int bless_img(const sgn_payload_opts *o, const uint8_t *der,
                    size_t der_len)
{
    vol_survey surveys[MAX_VOLS];
    size_t n = 0;
    size_t i;
    int payloads = 0;
    int signed_n = 0;
    int already_n = 0;
    int rc;

    rc = survey_image(o->path, o->work, surveys, MAX_VOLS, &n, &payloads);
    if (rc != SGN_OK)
        return rc;

    for (i = 0; i < n; i++) {
        const char *no = sgn_payload_refusal(surveys[i].fam);

        if (no == NULL)
            continue;
        sgn_set_error(SGN_ERR_FORMAT, "'%s' looks like %s (%s).", o->path,
                      family_by_id(surveys[i].fam)->name, no);
        return SGN_ERR_FORMAT;
    }
    if (payloads == 0) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "no /" PAYLOAD_MEMBER " in any filesystem inside '%s', "
                      "so there is no payload to sign (%zu volume(s) looked "
                      "at)", o->path, n);
        return SGN_ERR_FORMAT;
    }

    announce_head(o->path);
    (void)printf("  the file itself is a disk image; what is written to is "
                 "inside it:\n");
    for (i = 0; i < n; i++) {
        if (!surveys[i].has_payload)
            continue;
        /* Partition number AND byte offset: the number is what a user reads
         * in their own partition editor, and the offset is what they can
         * check with `mount -o loop,offset=`. The other loaders beside it
         * (BOOTIA32, BOOTAA64) are not touched, this being an x64 loader
         * (DESIGN Section 1). */
        (void)printf("    partition %u at byte %llu  ->  /" PAYLOAD_MEMBER
                     "   [%s]\n", surveys[i].vol.number,
                     (unsigned long long)surveys[i].vol.off,
                     family_by_id(surveys[i].fam)->name);
    }
    if (payloads > 1)
        (void)printf("  Every one of them, because the firmware picks "
                     "whichever volume its\n  partition driver surfaces "
                     "first, and they must agree.\n");
    announce_tail();
    if (o->dry_run) {
        (void)printf("would sign %d payload(s) inside %s\n", payloads,
                     o->path);
        return SGN_OK;
    }

    for (i = 0; i < n; i++) {
        char member[PATH_MAX];
        fatvol v;
        int already = 0;

        if (!surveys[i].has_payload)
            continue;
        rc = fatvol_open(&v, o->path, surveys[i].vol.off, surveys[i].vol.len,
                         o->work, 1);
        if (rc != SGN_OK)
            return rc;
        if (snprintf(member, sizeof(member), "%s/" PAYLOAD_MEMBER, v.mnt) >=
            (int)sizeof(member)) {
            fatvol_close(&v);
            sgn_set_error(SGN_ERR_NOSPACE, "the mounted payload path is too "
                          "long");
            return SGN_ERR_NOSPACE;
        }
        rc = sign_in_place(member, o->key, o->cert, der, der_len,
                           &already);
        fatvol_close(&v);
        if (rc != SGN_OK)
            return rc;
        if (already)
            already_n++;
        else
            signed_n++;
        (void)printf("  volume %u  /" PAYLOAD_MEMBER "  %s\n",
                     surveys[i].vol.number,
                     already ? "already signed with this key, left alone"
                             : "signed");
    }

    (void)printf("blessed %s\n", o->path);
    (void)printf("  signed   %d payload(s)\n", signed_n);
    if (already_n != 0)
        (void)printf("  skipped  %d already signed with this key\n",
                     already_n);
    return SGN_OK;
}

int sgn_payload_bless(const sgn_payload_opts *o)
{
    uint8_t *der = NULL;
    size_t der_len = 0;
    int rc;

    if (o == NULL || o->path == NULL || o->key == NULL || o->cert == NULL ||
        o->work == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "sgn_payload_bless: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    /* Up front, as the ISO path does it: a missing sbverify three steps in is
     * a worse message than a missing sbverify now. Both ship in sbsigntools,
     * so a machine that can bless an ISO can bless a payload. */
    if (!sgn_have_program("sbsign") || !sgn_have_program("sbverify")) {
        sgn_set_error(SGN_ERR_EXEC,
                      "need sbsign and sbverify on PATH (sbsign appends the "
                      "signature, sbverify counts what the payload carries "
                      "before and after); both ship in sbsigntools");
        return SGN_ERR_EXEC;
    }
    if (!sgn_file_exists(o->key) || !sgn_file_exists(o->cert)) {
        sgn_set_error(SGN_ERR_IO, "MOK key or certificate not found ('%s', "
                      "'%s')", o->key, o->cert);
        return SGN_ERR_IO;
    }
    if (!sgn_file_exists(o->path)) {
        sgn_set_error(SGN_ERR_IO, "no such payload: '%s'", o->path);
        return SGN_ERR_IO;
    }

    /* Read once, here, rather than per payload: an image can hold several,
     * and "which certificate is this run about" must not be answerable
     * differently for two of them. */
    rc = sgn_payload_cert_der(o->cert, &der, &der_len);
    if (rc != SGN_OK)
        return rc;

    switch (sgn_payload_shape_of(o->path)) {
    case SGN_PAYLOAD_EFI:
        rc = bless_efi(o, der, der_len);
        break;
    case SGN_PAYLOAD_IMG:
        rc = bless_img(o, der, der_len);
        break;
    case SGN_PAYLOAD_NONE:
    default:
        sgn_set_error(SGN_ERR_INTERNAL,
                      "'%s' is not a bare EFI payload", o->path);
        rc = SGN_ERR_INTERNAL;
        break;
    }
    free(der);
    return rc;
}
