/* falcon-boot-sign: build a UKI from sd-stub, a kernel and an initrd.
 * uki.h has the rules and why this replaced ukify. */

#include <sign/uki.h>
#include <sign/sign.h>

#include <pe/pe.h>
#include <gz/gz.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* COFF header, from its start (after "PE\0\0"). */
#define COFF_NSECT      2u
#define COFF_SYMPTR     8u

/* PE32+ optional header. */
#define OPT_INITDATA    8u
#define OPT_SECTALIGN   32u
#define OPT_FILEALIGN   36u
#define OPT_IMAGESIZE   56u
#define OPT_HDRSIZE     60u
#define OPT_CHECKSUM    64u
#define OPT_DLLCHAR     70u
#define DLLCHAR_NX      0x0100u

/* Section header. */
#define S_VSIZE         8u
#define S_VADDR         12u
#define S_RAWSIZE       16u
#define S_RAWPTR        20u
#define S_CHARS         36u

#define SCN_CODE        0x00000020u
#define SCN_IDATA       0x00000040u
#define SCN_READ        0x40000000u

/* sd-stub is about 140 KB; this is a bound on a file the user can point
 * --stub at, not an estimate of it. */
#define STUB_MAX        ((size_t)16 << 20)
/* The bzImage headers and PE section table sit in the first few KB. */
#define KERNEL_HEAD     ((size_t)64 * 1024u)
#define SBAT_MAX        ((size_t)64 * 1024u)
#define OSREL_MAX       ((size_t)64 * 1024u)
#define COPY_CHUNK      ((size_t)1 << 20)

#define SBAT_HEADER \
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* `a` is a power of two, checked by the caller. */
static uint64_t round_up(uint64_t x, uint32_t a)
{
    return (x + a - 1u) & ~((uint64_t)a - 1u);
}

static int pow2(uint32_t a)
{
    return a != 0u && (a & (a - 1u)) == 0u;
}

/* ------------------------------------------------------------ uname */

int uki_uname_generic(const uint8_t *k, size_t len, char *out, size_t cap)
{
    static const char banner[] = "Linux version ";
    const size_t bl = sizeof(banner) - 1u;
    size_t i;

    if (k == NULL || out == NULL || cap == 0u)
        return 0;
    for (i = 0; i + bl < len; i++) {
        size_t v = i + bl;
        size_t e = v;

        if (memcmp(k + i, banner, bl) != 0 || k[v] < '0' || k[v] > '9')
            continue;
        while (e < len && k[e] > ' ' && k[e] < 0x7fu)
            e++;
        if (e + 1u >= len || k[e] != ' ' || k[e + 1u] != '(')
            continue;
        if (e - v + 1u > cap || e - v > UKI_UNAME_MAX)
            return 0;
        memcpy(out, k + v, e - v);
        out[e - v] = '\0';
        return 1;
    }
    return 0;
}

static int version_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '+' || c == '-';
}

int uki_uname(const uint8_t *k, size_t len, char *out, size_t cap)
{
    char text[UKI_UNAME_MAX + 1u];
    size_t start;
    size_t n;
    size_t v;
    size_t i;

    if (k == NULL || out == NULL || cap == 0u || len < 0x210u)
        return 0;
    if (memcmp(k + 0x202u, "HdrS", 4) != 0)
        return 0;
    start = 0x200u + rd16(k + 0x20Eu);
    if (start >= len)
        return 0;

    /* ukify reads 128 bytes and cuts at the first NUL. */
    n = len - start;
    if (n > UKI_UNAME_MAX)
        n = UKI_UNAME_MAX;
    memcpy(text, k + start, n);
    text[n] = '\0';

    /* "<version> (<builder, no spaces>) #...", anchored at the start: the
     * shape ukify's VERSION_PATTERN accepts. */
    for (v = 0; version_char(text[v]); v++)
        ;
    if (v == 0u || text[v] != ' ' || text[v + 1u] != '(')
        return 0;
    i = v + 2u;
    if (text[i] == ')' || text[i] == ' ' || text[i] == '\0')
        return 0;
    while (text[i] != ')' && text[i] != ' ' && text[i] != '\0')
        i++;
    if (text[i] != ')' || text[i + 1u] != ' ' || text[i + 2u] != '#')
        return 0;

    if (v >= cap)
        return 0;
    memcpy(out, text, v);
    out[v] = '\0';
    return 1;
}

/* ------------------------------------------------------------ cmdline */

/* Python's str.split() whitespace, restricted to ASCII: a cmdline is ASCII. */
static int py_space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f);
}

void uki_cmdline_norm(const char *in, char *out)
{
    size_t o = 0;
    size_t i = 0;

    for (;;) {
        while (in[i] != '\0' && py_space(in[i]))
            i++;
        if (in[i] == '\0')
            break;
        if (o > 0u)
            out[o++] = ' ';
        while (in[i] != '\0' && !py_space(in[i]))
            out[o++] = in[i++];
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------ sbat */

static int append(char *out, size_t cap, size_t *at, const char *s, size_t n)
{
    if (n + 1u > cap - *at) {
        sgn_set_error(SGN_ERR_NOSPACE, "merged .sbat exceeds %zu bytes", cap);
        return SGN_ERR_NOSPACE;
    }
    memcpy(out + *at, s, n);
    out[*at + n] = '\n';
    *at += n + 1u;
    return SGN_OK;
}

int uki_sbat_merge(const char *const *in, const size_t *in_len, size_t n,
                   char *out, size_t cap, size_t *out_len)
{
    size_t at = 0;
    size_t k;
    int rc;

    if (out == NULL || out_len == NULL || cap == 0u) {
        sgn_set_error(SGN_ERR_INTERNAL, "uki_sbat_merge: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    rc = append(out, cap, &at, SBAT_HEADER, strlen(SBAT_HEADER));
    if (rc != SGN_OK)
        return rc;

    for (k = 0; k < n; k++) {
        const char *s = in[k];
        size_t len = in_len[k];
        size_t pos = 0;
        int first = 1;

        if (s == NULL)
            continue;
        while (len > 0u && s[len - 1u] == '\0')
            len--;
        /* Python's splitlines: \n, \r\n and \r all end a line, and a final
         * terminator does not start an empty one. */
        while (pos < len) {
            size_t e = pos;
            size_t next;

            while (e < len && s[e] != '\n' && s[e] != '\r')
                e++;
            next = e;
            if (next < len)
                next += (s[next] == '\r' && next + 1u < len &&
                         s[next + 1u] == '\n') ? 2u : 1u;

            if (first) {
                if (e - pos < 5u || memcmp(s + pos, "sbat,", 5) != 0)
                    break;          /* not an SBAT section: skipped whole */
                first = 0;
            } else {
                rc = append(out, cap, &at, s + pos, e - pos);
                if (rc != SGN_OK)
                    return rc;
            }
            pos = next;
        }
    }
    *out_len = at;
    return SGN_OK;
}

/* ------------------------------------------------------------ layout */

static int refuse(const char *why)
{
    sgn_set_error(SGN_ERR_FORMAT, "sd-stub: %s", why);
    return SGN_ERR_FORMAT;
}

static int sect_is(const uint8_t *s, const char *name)
{
    char n[PE_SECT_NAME + 1u];

    memcpy(n, s, PE_SECT_NAME);
    n[PE_SECT_NAME] = '\0';
    return strcmp(n, name) == 0;
}

int uki_layout(uint8_t *img, size_t img_len, uki_part *parts, size_t n,
               const char *sbat, size_t sbat_len, int clear_nx,
               uint64_t *out_len)
{
    pe_hdr h;
    size_t dir_off;
    uint32_t sig_rva;
    uint32_t sig_size;
    uint32_t sa;
    uint32_t fa;
    uint64_t hdrs;
    uint64_t first_raw = UINT64_MAX;
    uint64_t end;
    uint64_t vend;
    uint64_t initdata;
    uint16_t nsect;
    uint8_t *sbat_hdr = NULL;
    uint8_t *opt;
    int sbat_part = 0;
    size_t i;

    if (img == NULL || out_len == NULL || (n > 0u && parts == NULL) ||
        sbat == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "uki_layout: bad arguments");
        return SGN_ERR_INTERNAL;
    }
    if (pe_parse(img, img_len, &h) != PE_OK || h.magic != PE_MAGIC_PE32P ||
        h.opt_size < OPT_DLLCHAR + 2u || h.opt + h.opt_size > img_len)
        return refuse("not a PE32+ image");
    if (pe_dir(&h, img, PE_DIR_SECURITY, &dir_off, &sig_rva, &sig_size) ==
            PE_OK && sig_rva != 0u)
        return refuse("already signed; wrap an unsigned stub");
    (void)dir_off;
    (void)sig_size;
    if (rd32(img + h.coff + COFF_SYMPTR) != 0u)
        return refuse("carries a COFF symbol table (an unstripped stub)");

    opt = img + h.opt;
    nsect = h.nsect;
    if (nsect == 0u ||
        h.sect_table + (size_t)nsect * PE_SECT_ENTRY > img_len)
        return refuse("section table missing or truncated");
    sa = rd32(opt + OPT_SECTALIGN);
    fa = rd32(opt + OPT_FILEALIGN);
    if (!pow2(sa) || !pow2(fa))
        return refuse("alignment is not a power of two");

    hdrs = round_up(rd32(opt + OPT_HDRSIZE), fa);

    for (i = 0; i < nsect; i++) {
        uint8_t *s = img + h.sect_table + i * PE_SECT_ENTRY;
        uint32_t ptr = rd32(s + S_RAWPTR);
        uint32_t size = rd32(s + S_RAWSIZE);

        if (ptr % fa != 0u || size % fa != 0u)
            return refuse("section raw data is not FileAlignment-aligned");
        if ((uint64_t)ptr + size > img_len)
            return refuse("a section runs past the end of the file");
        if (size != 0u && ptr < first_raw)
            first_raw = ptr;
        if (sect_is(s, ".sbat"))
            sbat_hdr = s;
    }
    if (hdrs > first_raw)
        return refuse("headers overlap the first section");
    if (sbat_hdr == NULL)
        return refuse("has no .sbat section");
    /* A merge longer than the stub's section goes in a .sbat of its own,
     * which the caller passes as a part; the stub's is then renamed, so
     * shim finds one .sbat (ukify does the same). */
    for (i = 0; i < n; i++)
        if (strcmp(parts[i].name, ".sbat") == 0)
            sbat_part = 1;
    if (sbat_len > rd32(sbat_hdr + S_RAWSIZE) && !sbat_part) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "merged .sbat is %zu bytes, the stub's section holds %u",
                      sbat_len, rd32(sbat_hdr + S_RAWSIZE));
        return SGN_ERR_FORMAT;
    }

    {
        const uint8_t *last = img + h.sect_table +
                              (size_t)(nsect - 1u) * PE_SECT_ENTRY;

        vend = (uint64_t)rd32(last + S_VADDR) + rd32(last + S_VSIZE);
    }
    end = img_len;
    initdata = rd32(opt + OPT_INITDATA);

    for (i = 0; i < n; i++) {
        size_t hdr = h.sect_table + (size_t)nsect * PE_SECT_ENTRY;
        uint8_t *s = img + hdr;
        size_t name_len = strlen(parts[i].name);
        uint64_t raw_ptr = round_up(end, fa);
        uint64_t raw_size = round_up(parts[i].len, fa);
        uint64_t va = round_up(vend, sa);

        if (hdr + PE_SECT_ENTRY > hdrs) {
            sgn_set_error(SGN_ERR_FORMAT,
                          "sd-stub: no room in its headers for section %s",
                          parts[i].name);
            return SGN_ERR_FORMAT;
        }
        if (name_len > PE_SECT_NAME || nsect == UINT16_MAX)
            return refuse("section name too long or too many sections");
        if (raw_ptr + raw_size > UINT32_MAX || va + parts[i].len > UINT32_MAX ||
            initdata + parts[i].len > UINT32_MAX) {
            sgn_set_error(SGN_ERR_FORMAT,
                          "%s is too large for a PE image (%llu bytes)",
                          parts[i].name, (unsigned long long)parts[i].len);
            return SGN_ERR_FORMAT;
        }

        memset(s, 0, PE_SECT_ENTRY);
        memcpy(s, parts[i].name, name_len);
        wr32(s + S_VSIZE, (uint32_t)parts[i].len);
        wr32(s + S_VADDR, (uint32_t)va);
        wr32(s + S_RAWSIZE, (uint32_t)raw_size);
        wr32(s + S_RAWPTR, (uint32_t)raw_ptr);
        wr32(s + S_CHARS, SCN_READ | (parts[i].code ? SCN_CODE : SCN_IDATA));

        parts[i].raw_ptr = (uint32_t)raw_ptr;
        parts[i].raw_size = (uint32_t)raw_size;
        nsect++;
        initdata += parts[i].len;
        end = raw_ptr + raw_size;
        vend = va + parts[i].len;
    }

    /* The stub's own .sbat, rewritten where it stands, or renamed when the
     * merge is a part of its own. */
    if (sbat_part) {
        /* A section name fills its eight bytes; it has no terminator. */
        static const uint8_t renamed[PE_SECT_NAME] = {
            '.', 's', 'b', 'a', 't', '.', 's', 't'
        };

        memcpy(sbat_hdr, renamed, sizeof renamed);
    } else {
        uint32_t ptr = rd32(sbat_hdr + S_RAWPTR);
        uint32_t size = rd32(sbat_hdr + S_RAWSIZE);

        memcpy(img + ptr, sbat, sbat_len);
        memset(img + ptr + sbat_len, 0, size - sbat_len);
        wr32(sbat_hdr + S_VSIZE, (uint32_t)sbat_len);
    }

    wr16(img + h.coff + COFF_NSECT, nsect);
    wr32(opt + OPT_INITDATA, (uint32_t)initdata);
    wr32(opt + OPT_HDRSIZE, (uint32_t)hdrs);
    wr32(opt + OPT_CHECKSUM, 0u);
    wr32(opt + OPT_IMAGESIZE, (uint32_t)round_up(vend, sa));
    if (clear_nx)
        wr16(opt + OPT_DLLCHAR,
             (uint16_t)(rd16(opt + OPT_DLLCHAR) & ~DLLCHAR_NX));

    *out_len = end;
    return SGN_OK;
}

/* ------------------------------------------------------------ files */

static int write_all(int fd, const void *buf, size_t n, const char *path)
{
    const uint8_t *p = buf;

    while (n > 0u) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            sgn_set_error_errno(SGN_ERR_IO, "writing %s", path);
            return SGN_ERR_IO;
        }
        p += w;
        n -= (size_t)w;
    }
    return SGN_OK;
}

static int write_zeros(int fd, uint64_t n, const char *path)
{
    static const uint8_t zeros[4096];

    while (n > 0u) {
        size_t c = (n > sizeof(zeros)) ? sizeof(zeros) : (size_t)n;
        int rc = write_all(fd, zeros, c, path);

        if (rc != SGN_OK)
            return rc;
        n -= c;
    }
    return SGN_OK;
}

/* Exactly `len` bytes of `src` onto fd. A file that changed length since it
 * was measured is an error, not a short section. */
static int copy_file(int fd, const char *out, const char *src, uint64_t from,
                     uint64_t len, uint8_t *buf)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    uint64_t left = len;
    int rc = SGN_OK;

    if (in < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "opening %s", src);
        return SGN_ERR_IO;
    }
    if (lseek(in, (off_t)from, SEEK_SET) < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "seeking in %s", src);
        (void)close(in);
        return SGN_ERR_IO;
    }
    while (left > 0u) {
        size_t want = (left > COPY_CHUNK) ? COPY_CHUNK : (size_t)left;
        ssize_t r = read(in, buf, want);

        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            sgn_set_error_errno(SGN_ERR_IO, "reading %s", src);
            rc = SGN_ERR_IO;
            break;
        }
        if (r == 0) {
            sgn_set_error(SGN_ERR_IO, "%s shrank while being read", src);
            rc = SGN_ERR_IO;
            break;
        }
        rc = write_all(fd, buf, (size_t)r, out);
        if (rc != SGN_OK)
            break;
        left -= (uint64_t)r;
    }
    (void)close(in);
    return rc;
}

static int read_at(int fd, uint64_t off, void *buf, size_t n, const char *path)
{
    size_t got = 0;

    while (got < n) {
        ssize_t r = pread(fd, (uint8_t *)buf + got, n - got,
                          (off_t)(off + got));

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            sgn_set_error_errno(SGN_ERR_IO, "reading %s", path);
            return SGN_ERR_IO;
        }
        got += (size_t)r;
    }
    return SGN_OK;
}

/* What a kernel that is itself a stub image (a .linux inside) brings beside
 * that .linux: its devicetrees, the table sd-stub picks one by (systemd 257
 * reads both), and firmware blobs. Fedora's arm64 kernel is such an image.
 * Wrapped whole, its own stub would start the kernel inside through shim
 * and be refused, as nothing signed that kernel; unwrapped, our stub starts
 * it and the wrap's signature covers it, as for any kernel. */
static const char *const g_carried[] = { ".dtb", ".dtbauto", ".hwids",
                                         ".efifw" };
#define CARRY_MAX 64u

typedef struct {
    char     name[PE_SECT_NAME + 1u];
    uint64_t off;
    uint64_t len;
} carried_sect;

/* What the kernel contributes besides its bytes: where they start in the
 * file (a stub image's .linux, or 0), its machine, whether it is NX_COMPAT,
 * its own .sbat if it has one, its version for .uname, and what a stub image
 * carries beside it. */
typedef struct {
    uint64_t off;
    uint64_t size;
    uint16_t machine;
    int      nx;
    char    *sbat;
    size_t   sbat_len;
    char     uname[UKI_UNAME_MAX + 1u];
    int      have_uname;
    carried_sect carry[CARRY_MAX];
    size_t   ncarry;
} kernel_facts;

/* uki_uname_generic over the whole kernel, a window at a time, each window
 * overlapping the last by enough to hold a banner split across them. */
#define SCAN_WINDOW ((size_t)1024 * 1024)
#define SCAN_OVERLAP ((size_t)256)

static int uname_scan(int fd, uint64_t base, uint64_t size, char *out,
                      size_t cap)
{
    uint8_t *buf = malloc(SCAN_WINDOW);
    uint64_t at = 0;
    int found = 0;

    if (buf == NULL)
        return 0;
    while (at < size && !found) {
        size_t want = (size - at) < SCAN_WINDOW ? (size_t)(size - at)
                                                : SCAN_WINDOW;
        ssize_t r = pread(fd, buf, want, (off_t)(base + at));

        if (r <= 0)
            break;
        found = uki_uname_generic(buf, (size_t)r, out, cap);
        if ((size_t)r < SCAN_WINDOW)
            break;
        at += SCAN_WINDOW - SCAN_OVERLAP;
    }
    free(buf);
    return found;
}

/* A stub image's .linux, its .uname, and what g_carried names, into kf.
 * Nothing when the image has no .linux. */
static int unwrap_stub_image(int fd, const char *path, const pe_hdr *h,
                             const uint8_t *head, size_t head_len,
                             kernel_facts *kf)
{
    size_t loff;
    size_t lsize;
    uint16_t i;
    size_t k;

    if (pe_find_section(h, head, (size_t)kf->size, ".linux", &loff,
                        &lsize) != PE_OK)
        return SGN_OK;
    if (h->sect_table + (size_t)h->nsect * PE_SECT_ENTRY > head_len) {
        sgn_set_error(SGN_ERR_FORMAT, "%s: section table past its first "
                      "%zu bytes", path, head_len);
        return SGN_ERR_FORMAT;
    }
    for (i = 0; i < h->nsect; i++) {
        const uint8_t *s = head + h->sect_table + (size_t)i * PE_SECT_ENTRY;
        uint32_t vs = rd32(s + S_VSIZE);
        pe_sect ps;
        uint64_t len;

        if (pe_section(h, head, i, &ps) != PE_OK ||
            (uint64_t)ps.raw_ptr + ps.raw_size > kf->size)
            continue;
        /* VirtualSize is the content, SizeOfRawData that padded. */
        len = (vs != 0u && vs < ps.raw_size) ? vs : ps.raw_size;
        if (len == 0u)
            continue;
        if (strcmp(ps.name, ".linux") == 0) {
            kf->off = ps.raw_ptr;
            kf->size = len;
        } else if (strcmp(ps.name, ".uname") == 0 && len <= UKI_UNAME_MAX) {
            if (read_at(fd, ps.raw_ptr, kf->uname, (size_t)len, path) !=
                SGN_OK)
                return SGN_ERR_IO;
            kf->uname[len] = '\0';
            kf->have_uname = 1;
        } else {
            for (k = 0; k < sizeof g_carried / sizeof g_carried[0]; k++) {
                if (strcmp(ps.name, g_carried[k]) != 0)
                    continue;
                if (kf->ncarry == CARRY_MAX) {
                    sgn_set_error(SGN_ERR_FORMAT, "%s carries more than %u "
                                  "devicetree sections", path, CARRY_MAX);
                    return SGN_ERR_FORMAT;
                }
                memcpy(kf->carry[kf->ncarry].name, ps.name,
                       sizeof kf->carry[kf->ncarry].name);
                kf->carry[kf->ncarry].off = ps.raw_ptr;
                kf->carry[kf->ncarry].len = len;
                kf->ncarry++;
            }
        }
    }
    (void)loff;
    (void)lsize;
    return SGN_OK;
}

static int read_kernel(const char *path, int bios_only, kernel_facts *kf)
{
    uint8_t *head = NULL;
    size_t head_len;
    struct stat st;
    pe_hdr h;
    size_t off;
    size_t size;
    int fd;
    int rc;

    memset(kf, 0, sizeof(*kf));
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "opening %s", path);
        return SGN_ERR_IO;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        sgn_set_error(SGN_ERR_IO, "%s is not a regular file", path);
        (void)close(fd);
        return SGN_ERR_IO;
    }
    kf->size = (uint64_t)st.st_size;
    if (kf->size < 0x400u) {
        sgn_set_error(SGN_ERR_FORMAT, "%s is too small to be a kernel", path);
        (void)close(fd);
        return SGN_ERR_FORMAT;
    }
    head_len = (kf->size < KERNEL_HEAD) ? (size_t)kf->size : KERNEL_HEAD;
    head = calloc(1, head_len);
    if (head == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        (void)close(fd);
        return SGN_ERR_IO;
    }
    rc = read_at(fd, 0, head, head_len, path);
    if (rc != SGN_OK)
        goto out;

    /* A bzImage with no EFI stub, asked for: nothing of it is PE, so it
     * carries no NX flag and no .sbat; its setup header is what falcon-bios
     * boots, and its version is read from there as for any kernel. */
    if (bios_only && pe_parse(head, head_len, &h) != PE_OK) {
        if (memcmp(head + 0x202, "HdrS", 4) != 0) {
            sgn_set_error(SGN_ERR_FORMAT,
                          "%s is not a bzImage (no HdrS setup header)", path);
            rc = SGN_ERR_FORMAT;
            goto out;
        }
        kf->have_uname = uki_uname(head, head_len, kf->uname,
                                   sizeof(kf->uname));
        goto out;
    }

    /* sd-stub starts the kernel through its PE entry point; a kernel without
     * one (no EFI stub, or compressed as a whole) would build and not boot. */
    if (pe_parse(head, head_len, &h) != PE_OK || h.magic != PE_MAGIC_PE32P ||
        h.opt_size < OPT_DLLCHAR + 2u || h.opt + h.opt_size > head_len) {
        sgn_set_error(SGN_ERR_FORMAT,
                      "%s is not an EFI-stub kernel (no PE32+ header); "
                      "--bios-only wraps one for falcon-bios alone", path);
        rc = SGN_ERR_FORMAT;
        goto out;
    }
    rc = unwrap_stub_image(fd, path, &h, head, head_len, kf);
    if (rc != SGN_OK)
        goto out;
    if (kf->off != 0u) {
        size_t inner = (kf->size < KERNEL_HEAD) ? (size_t)kf->size
                                                : KERNEL_HEAD;

        memset(head, 0, head_len);
        rc = read_at(fd, kf->off, head, inner, path);
        if (rc != SGN_OK)
            goto out;
        head_len = inner;
        if (pe_parse(head, head_len, &h) != PE_OK ||
            h.magic != PE_MAGIC_PE32P || h.opt_size < OPT_DLLCHAR + 2u ||
            h.opt + h.opt_size > head_len) {
            sgn_set_error(SGN_ERR_FORMAT, "%s: the kernel in its .linux is "
                          "not an EFI-stub kernel", path);
            rc = SGN_ERR_FORMAT;
            goto out;
        }
    }
    kf->nx = (rd16(head + h.opt + OPT_DLLCHAR) & DLLCHAR_NX) != 0u;
    kf->machine = h.machine;

    switch (pe_find_section(&h, head, (size_t)kf->size, ".sbat", &off,
                            &size)) {
    case PE_OK:
        if (size > SBAT_MAX) {
            sgn_set_error(SGN_ERR_FORMAT, "%s: .sbat is %zu bytes", path, size);
            rc = SGN_ERR_FORMAT;
            goto out;
        }
        kf->sbat = malloc(size > 0u ? size : 1u);
        if (kf->sbat == NULL) {
            sgn_set_error(SGN_ERR_IO, "out of memory");
            rc = SGN_ERR_IO;
            goto out;
        }
        rc = read_at(fd, kf->off + off, kf->sbat, size, path);
        if (rc != SGN_OK)
            goto out;
        kf->sbat_len = size;
        break;
    case PE_ERR_ABSENT:
        break;
    default:
        sgn_set_error(SGN_ERR_FORMAT, "%s: unreadable PE section table", path);
        rc = SGN_ERR_FORMAT;
        goto out;
    }

    if (!kf->have_uname)
        kf->have_uname = uki_uname(head, head_len, kf->uname,
                                   sizeof(kf->uname));
    if (!kf->have_uname)
        kf->have_uname = uname_scan(fd, kf->off, kf->size, kf->uname,
                                    sizeof(kf->uname));

out:
    free(head);
    (void)close(fd);
    if (rc != SGN_OK) {
        free(kf->sbat);
        kf->sbat = NULL;
    }
    return rc;
}

int uki_build(const uki_spec *spec)
{
    static const char uki_sbat[] = SBAT_HEADER "\n" UKI_SBAT_UKI_LINE "\n";
    kernel_facts kf;
    const char *stub_path;
    char *stub = NULL;
    size_t stub_len = 0;
    uint64_t initrd_size = 0;
    char *sbat = NULL;
    size_t sbat_len = 0;
    int sbat_own = 0;
    char *cmdline = NULL;
    uki_part parts[6u + CARRY_MAX];
    const char *src[6u + CARRY_MAX];
    const char *mem[6u + CARRY_MAX];
    uint64_t from[6u + CARRY_MAX];
    size_t n = 0;
    uint64_t out_len = 0;
    uint64_t pos;
    uint8_t *buf = NULL;
    pe_hdr h;
    int fd = -1;
    int rc;
    size_t i;

    if (spec == NULL || spec->kernel == NULL ||
        spec->cmdline == NULL || spec->osrel == NULL || spec->out == NULL) {
        sgn_set_error(SGN_ERR_INTERNAL, "uki_build: incomplete spec");
        return SGN_ERR_INTERNAL;
    }

    rc = read_kernel(spec->kernel, spec->bios_only, &kf);
    if (rc != SGN_OK)
        return rc;
    stub_path = spec->stub;
    if (stub_path == NULL) {
        /* A bzImage with no EFI stub is x86's; falcon-bios unwraps it and
         * the stub around it never runs. */
        stub_path = uki_default_stub(spec->bios_only && kf.machine == 0
                                         ? (uint16_t)PE_MACHINE_AMD64
                                         : kf.machine);
        if (stub_path == NULL) {
            sgn_set_error(SGN_ERR_FORMAT, "%s: no sd-stub for its machine "
                          "(0x%04x); pass --stub", spec->kernel,
                          (unsigned)kf.machine);
            rc = SGN_ERR_FORMAT;
            goto out;
        }
    }
    if (spec->initrd != NULL) {
        rc = sgn_file_size(spec->initrd, &initrd_size);
        if (rc != SGN_OK)
            goto out;
    }
    rc = sgn_read_file(stub_path, &stub, &stub_len, STUB_MAX);
    if (rc != SGN_OK)
        goto out;
    cmdline = malloc(strlen(spec->cmdline) + 1u);
    if (cmdline == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        rc = SGN_ERR_IO;
        goto out;
    }
    uki_cmdline_norm(spec->cmdline, cmdline);

    {
        const char *in[3];
        size_t in_len[3];
        size_t soff;
        size_t ssize;

        if (pe_parse(stub, stub_len, &h) != PE_OK ||
            pe_find_section(&h, stub, stub_len, ".sbat", &soff, &ssize) !=
                PE_OK) {
            sgn_set_error(SGN_ERR_FORMAT, "%s: not a PE image with a .sbat "
                          "section", stub_path);
            rc = SGN_ERR_FORMAT;
            goto out;
        }
        if (!spec->bios_only && h.machine != kf.machine) {
            sgn_set_error(SGN_ERR_FORMAT, "%s is for machine 0x%04x and %s "
                          "for 0x%04x; a stub starts only its own machine's "
                          "kernel", stub_path, (unsigned)h.machine,
                          spec->kernel, (unsigned)kf.machine);
            rc = SGN_ERR_FORMAT;
            goto out;
        }
        in[0] = stub + soff;
        in_len[0] = ssize;
        in[1] = kf.sbat;
        in_len[1] = kf.sbat_len;
        in[2] = uki_sbat;
        in_len[2] = sizeof(uki_sbat) - 1u;
        /* The merge goes back into the stub's own .sbat when it fits, and
         * into a .sbat of its own when not: a kernel that is itself a
         * stub image (Fedora's arm64 one carries its devicetrees that way)
         * brings that stub's lines too. */
        sbat = malloc(SBAT_MAX);
        if (sbat == NULL) {
            sgn_set_error(SGN_ERR_IO, "out of memory");
            rc = SGN_ERR_IO;
            goto out;
        }
        rc = uki_sbat_merge(in, in_len, 3, sbat, SBAT_MAX, &sbat_len);
        if (rc != SGN_OK)
            goto out;
        sbat_own = sbat_len > ssize;
    }

    /* ukify's order. */
#define ADD(nm, length, is_code, path, at, text) do { \
        parts[n].name = (nm); parts[n].len = (length); \
        parts[n].code = (is_code); src[n] = (path); from[n] = (at); \
        mem[n] = (text); n++; \
    } while (0)
    ADD(".osrel", strlen(spec->osrel), 0, NULL, 0u, spec->osrel);
    if (sbat_own)
        ADD(".sbat", sbat_len, 0, NULL, 0u, sbat);
    ADD(".cmdline", strlen(cmdline), 0, NULL, 0u, cmdline);
    for (i = 0; i < kf.ncarry; i++)
        ADD(kf.carry[i].name, kf.carry[i].len, 0, spec->kernel,
            kf.carry[i].off, NULL);
    if (kf.have_uname)
        ADD(".uname", strlen(kf.uname), 0, NULL, 0u, kf.uname);
    ADD(".linux", kf.size, 1, spec->kernel, kf.off, NULL);
    if (spec->initrd != NULL)
        ADD(".initrd", initrd_size, 0, spec->initrd, 0u, NULL);
#undef ADD

    rc = uki_layout((uint8_t *)stub, stub_len, parts, n, sbat, sbat_len,
                    !kf.nx, &out_len);
    if (rc != SGN_OK)
        goto out;

    buf = malloc(COPY_CHUNK);
    if (buf == NULL) {
        sgn_set_error(SGN_ERR_IO, "out of memory");
        rc = SGN_ERR_IO;
        goto out;
    }
    fd = open(spec->out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        sgn_set_error_errno(SGN_ERR_IO, "creating %s", spec->out);
        rc = SGN_ERR_IO;
        goto out;
    }
    rc = write_all(fd, stub, stub_len, spec->out);
    pos = stub_len;
    for (i = 0; rc == SGN_OK && i < n; i++) {
        rc = write_zeros(fd, parts[i].raw_ptr - pos, spec->out);
        if (rc != SGN_OK)
            break;
        if (mem[i] != NULL)
            rc = write_all(fd, mem[i], (size_t)parts[i].len, spec->out);
        else
            rc = copy_file(fd, spec->out, src[i], from[i], parts[i].len, buf);
        if (rc != SGN_OK)
            break;
        rc = write_zeros(fd, parts[i].raw_size - parts[i].len, spec->out);
        pos = (uint64_t)parts[i].raw_ptr + parts[i].raw_size;
    }
    if (rc == SGN_OK && pos != out_len) {
        sgn_set_error(SGN_ERR_INTERNAL, "UKI length %llu, planned %llu",
                      (unsigned long long)pos, (unsigned long long)out_len);
        rc = SGN_ERR_INTERNAL;
    }
    if (close(fd) != 0 && rc == SGN_OK) {
        sgn_set_error_errno(SGN_ERR_IO, "closing %s", spec->out);
        rc = SGN_ERR_IO;
    }
    fd = -1;
    if (rc != SGN_OK)
        (void)unlink(spec->out);

out:
    if (fd >= 0)
        (void)close(fd);
    free(buf);
    free(cmdline);
    free(sbat);
    free(stub);
    free(kf.sbat);
    return rc;
}

/* ------------------------------------------------------------ uki verb */

const char *uki_default_stub(uint16_t machine)
{
    if (machine == PE_MACHINE_AMD64)
        return UKI_STUB_X64;
    if (machine == PE_MACHINE_ARM64)
        return UKI_STUB_AA64;
    return NULL;
}

uint16_t uki_kernel_machine(const char *path)
{
    uint8_t head[KERNEL_HEAD];
    pe_hdr h;
    ssize_t r;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    r = pread(fd, head, sizeof(head), 0);
    (void)close(fd);
    if (r <= 0 || pe_parse(head, (size_t)r, &h) != PE_OK ||
        h.magic != PE_MAGIC_PE32P)
        return 0;
    return h.machine;
}

/* A gzip member's trailer holds its size mod 2^32; a kernel claiming more
 * than this is not one. */
#define GZ_KERNEL_MAX ((size_t)512 * 1024 * 1024)

int uki_kernel_unpack(const char *path)
{
    char *in = NULL;
    size_t in_len = 0;
    uint8_t *out = NULL;
    size_t want;
    size_t got = 0;
    int rc;

    rc = sgn_read_file(path, &in, &in_len, GZ_KERNEL_MAX);
    if (rc != SGN_OK)
        return rc;
    if (in_len < 18u || (uint8_t)in[0] != 0x1fu || (uint8_t)in[1] != 0x8bu) {
        free(in);
        return SGN_OK;
    }
    want = (size_t)(uint8_t)in[in_len - 4u] |
           ((size_t)(uint8_t)in[in_len - 3u] << 8) |
           ((size_t)(uint8_t)in[in_len - 2u] << 16) |
           ((size_t)(uint8_t)in[in_len - 1u] << 24);
    if (want == 0u || want > GZ_KERNEL_MAX) {
        free(in);
        sgn_set_error(SGN_ERR_FORMAT, "%s: a gzip member of %zu bytes is "
                      "not a kernel", path, want);
        return SGN_ERR_FORMAT;
    }
    out = malloc(want);
    if (out == NULL) {
        free(in);
        sgn_set_error(SGN_ERR_IO, "out of memory");
        return SGN_ERR_IO;
    }
    if (gz_member((const uint8_t *)in, in_len, out, want, &got) != GZ_OK ||
        got != want) {
        sgn_set_error(SGN_ERR_FORMAT, "%s: the gzip member does not unpack",
                      path);
        rc = SGN_ERR_FORMAT;
    } else {
        rc = sgn_write_file_atomic(path, out, got);
        if (rc == SGN_OK)
            sgn_log(SGN_LOG_INFO, "unpacked the kernel (%zu bytes)", got);
    }
    free(in);
    free(out);
    return rc;
}

static void uki_usage(FILE *f)
{
    (void)fprintf(f,
"usage: falcon-boot-sign uki --linux <kernel> --cmdline <string>\n"
"                            --os-release <file> --output <uki>\n"
"                            [--initrd <file>] [--stub <sd-stub>]\n"
"                            [--bios-only]\n"
"\n"
"Builds an unsigned UKI: sd-stub with .osrel, .cmdline, .uname (read from the\n"
"kernel), .linux and .initrd appended and .sbat merged, laid out as ukify\n"
"lays it out. This is the wrap `bless' makes, offered on its own for images\n"
"the tool does not bless, such as the signing appliance itself. Sign the\n"
"result with sbsign.\n"
"\n"
"  --stub <file>   default: the kernel's machine's, " UKI_STUB_X64 "\n"
"                  or " UKI_STUB_AA64 "\n"
"  --bios-only     the kernel is a bzImage with no EFI stub; the UKI boots\n"
"                  only where falcon-bios unwraps it, never through sd-stub\n");
}

int sgn_uki_main(int argc, char **argv)
{
    uki_spec spec;
    const char *osrel_path = NULL;
    char *osrel = NULL;
    size_t osrel_len = 0;
    int i;
    int rc;

    memset(&spec, 0, sizeof(spec));

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char **slot = NULL;

        if (strcmp(a, "--help") == 0) {
            uki_usage(stdout);
            return SGN_OK;
        }
        if (strcmp(a, "--bios-only") == 0) {
            spec.bios_only = 1;
            continue;
        }
        if (strcmp(a, "--linux") == 0)
            slot = &spec.kernel;
        else if (strcmp(a, "--initrd") == 0)
            slot = &spec.initrd;
        else if (strcmp(a, "--cmdline") == 0)
            slot = &spec.cmdline;
        else if (strcmp(a, "--os-release") == 0)
            slot = &osrel_path;
        else if (strcmp(a, "--stub") == 0)
            slot = &spec.stub;
        else if (strcmp(a, "--output") == 0)
            slot = &spec.out;
        if (slot == NULL || i + 1 >= argc) {
            sgn_log(SGN_LOG_ERROR, slot == NULL ? "unknown argument '%s'"
                                                : "'%s' needs a value", a);
            uki_usage(stderr);
            return SGN_ERR_ARG;
        }
        *slot = argv[++i];
    }
    if (spec.kernel == NULL || spec.cmdline == NULL || osrel_path == NULL ||
        spec.out == NULL) {
        sgn_log(SGN_LOG_ERROR,
                "--linux, --cmdline, --os-release and --output are required");
        uki_usage(stderr);
        return SGN_ERR_ARG;
    }

    rc = sgn_read_file(osrel_path, &osrel, &osrel_len, OSREL_MAX);
    if (rc == SGN_OK && strlen(osrel) != osrel_len) {
        sgn_set_error(SGN_ERR_FORMAT, "%s contains a NUL byte", osrel_path);
        rc = SGN_ERR_FORMAT;
    }
    if (rc == SGN_OK) {
        spec.osrel = osrel;
        rc = uki_build(&spec);
    }
    free(osrel);
    if (rc != SGN_OK)
        sgn_log(SGN_LOG_ERROR, "%s", sgn_error());
    return rc;
}
