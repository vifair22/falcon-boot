/* Tests for falcon-cli/src/sign/uki.c, the UKI builder that replaced ukify.
 *
 * The claim the module rests on is byte-identity with ukify, and that is
 * checked against the real thing by falcon-cli/tools/cmp-ukify.sh over real
 * kernels. What is covered here is what real inputs cannot reach: every stub
 * the layout refuses, the edges of the uname and .sbat parsers, and the build
 * end to end over a synthetic stub and kernel, read back through
 * falcon-core/src/pe, the reader the loader itself uses.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pe/pe.h>
#include <sign/sign.h>
#include <sign/uki.h>

/* ------------------------------------------------------------ fixtures */

#define LFANEW      0x80u
#define COFF        (LFANEW + 4u)
#define OPT         (LFANEW + 24u)
#define OPT_SIZE    0xF0u
#define TABLE       (OPT + OPT_SIZE)
#define STUB_LEN    0x800u
#define HDRS        0x400u

#define STUB_SBAT \
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n" \
    "stub,1,Test,stub,1,https://example.invalid\n"

static void wr16(uint8_t *p, uint32_t v)
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

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void sect(uint8_t *b, unsigned i, const char *name, uint32_t va,
                 uint32_t vs, uint32_t raw, uint32_t rawsz)
{
    uint8_t *s = b + TABLE + i * 40u;

    memcpy(s, name, strlen(name));
    wr32(s + 8, vs);
    wr32(s + 12, va);
    wr32(s + 16, rawsz);
    wr32(s + 20, raw);
    wr32(s + 36, 0x40000040u);
}

/* A PE32+ shaped like a stripped sd-stub: .text and .sbat, file-aligned, room
 * in the headers, CheckSum set so zeroing it shows. `nx` sets NX_COMPAT. */
static void build_pe(uint8_t *b, size_t len, int nx, int with_sbat)
{
    memset(b, 0, len);
    b[0] = 'M';
    b[1] = 'Z';
    wr32(b + 0x3C, LFANEW);
    memcpy(b + LFANEW, "PE\0\0", 4);
    wr16(b + COFF, 0x8664u);
    wr16(b + COFF + 2, with_sbat ? 2u : 1u);
    wr16(b + COFF + 16, OPT_SIZE);
    wr16(b + OPT, 0x20Bu);
    wr32(b + OPT + 8, 0x100u);
    wr32(b + OPT + 32, 0x1000u);
    wr32(b + OPT + 36, 0x200u);
    wr32(b + OPT + 56, 0x3000u);
    wr32(b + OPT + 60, HDRS);
    wr32(b + OPT + 64, 0x1234u);
    wr16(b + OPT + 70, nx ? 0x0160u : 0x0060u);
    wr32(b + OPT + 108, 16u);
    sect(b, 0, ".text", 0x1000u, 0x10u, 0x400u, 0x200u);
    if (with_sbat) {
        sect(b, 1, ".sbat", 0x2000u, (uint32_t)strlen(STUB_SBAT), 0x600u,
             0x200u);
        memcpy(b + 0x600, STUB_SBAT, strlen(STUB_SBAT));
    }
}

/* The bzImage real-mode header on top: HdrS, and the version string pointer
 * at 0x20E, which is relative to 0x200. */
static void add_bzimage(uint8_t *b, const char *version)
{
    memcpy(b + 0x202, "HdrS", 4);
    wr16(b + 0x20E, 0x100u);
    memcpy(b + 0x300, version, strlen(version) + 1u);
}

static uint8_t g_stub[STUB_LEN];

static int setup_stub(void **state)
{
    (void)state;
    build_pe(g_stub, sizeof(g_stub), 1, 1);
    return 0;
}

static char g_dir[64];

static int setup_dir(void **state)
{
    (void)state;
    (void)snprintf(g_dir, sizeof(g_dir), "/tmp/falcon-uki-test.XXXXXX");
    return mkdtemp(g_dir) != NULL ? 0 : -1;
}

static int teardown_dir(void **state)
{
    char cmd[128];

    (void)state;
    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    return system(cmd) == 0 ? 0 : -1;
}

static void put(const char *name, const void *buf, size_t len, char *path,
                size_t cap)
{
    FILE *f;

    (void)snprintf(path, cap, "%s/%s", g_dir, name);
    f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(buf, 1, len, f), len);
    assert_int_equal(fclose(f), 0);
}

/* ------------------------------------------------------------ uname */

static void test_uki_uname_reads_the_version(void **state)
{
    uint8_t k[0x400];
    char out[UKI_UNAME_MAX + 1u];

    (void)state;
    memset(k, 0, sizeof(k));
    add_bzimage(k, "6.18.2-arch2-1 (linux@archlinux) #1 SMP PREEMPT_DYNAMIC");
    assert_int_equal(uki_uname(k, sizeof(k), out, sizeof(out)), 1);
    assert_string_equal(out, "6.18.2-arch2-1");
}

static void test_uki_uname_needs_the_whole_shape(void **state)
{
    static const char *const bad[] = {
        "6.18.2-Arch (linux@archlinux) #1",     /* upper case in the version */
        "6.18.2 linux@archlinux) #1",           /* no opening paren */
        "6.18.2 (linux archlinux) #1",          /* space in the builder */
        "6.18.2 () #1",                         /* empty builder */
        "6.18.2 (linux@archlinux) SMP",         /* no # */
        "6.18.2 (linux@archlinux",              /* runs out */
        " (linux@archlinux) #1",                /* no version */
    };
    uint8_t k[0x400];
    char out[UKI_UNAME_MAX + 1u];
    size_t i;

    (void)state;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(k, 0, sizeof(k));
        add_bzimage(k, bad[i]);
        assert_int_equal(uki_uname(k, sizeof(k), out, sizeof(out)), 0);
    }
}

static void test_uki_uname_refuses_what_is_not_a_bzimage(void **state)
{
    uint8_t k[0x400];
    char out[8];

    (void)state;
    memset(k, 0, sizeof(k));
    assert_int_equal(uki_uname(k, sizeof(k), out, sizeof(out)), 0);
    add_bzimage(k, "6.18.2-arch2-1 (linux@archlinux) #1");
    /* The version does not fit the caller's buffer. */
    assert_int_equal(uki_uname(k, sizeof(k), out, sizeof(out)), 0);
    /* The string pointer leads past the end of what was read. */
    assert_int_equal(uki_uname(k, 0x300u, out, sizeof(out)), 0);
    assert_int_equal(uki_uname(k, 0x100u, out, sizeof(out)), 0);
    assert_int_equal(uki_uname(NULL, sizeof(k), out, sizeof(out)), 0);
}

/* ------------------------------------------------------------ sbat */

static void test_uki_sbat_merge_in_order(void **state)
{
    static const char kern[] =
        "sbat,1,SBAT Version,sbat,1,x\r\nlinux,1,L,linux,1,y\r\n\0\0\0";
    static const char uki[] = "sbat,1,x\nuki,1,UKI,uki,1,z\n";
    const char *in[4] = { STUB_SBAT, NULL, kern, uki };
    size_t len[4] = { strlen(STUB_SBAT), 0, sizeof(kern) - 1u,
                      sizeof(uki) - 1u };
    char out[512];
    size_t n = 0;

    (void)state;
    assert_int_equal(uki_sbat_merge(in, len, 4, out, sizeof(out), &n), SGN_OK);
    out[n] = '\0';
    assert_string_equal(out,
        "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
        "stub,1,Test,stub,1,https://example.invalid\n"
        "linux,1,L,linux,1,y\n"
        "uki,1,UKI,uki,1,z\n");
}

static void test_uki_sbat_merge_skips_what_is_not_sbat(void **state)
{
    static const char junk[] = "hello\nstub,1,Test\n";
    static const char bare_cr[] = "sbat,1,x\rone,1\rtwo,1";
    const char *in[2] = { junk, bare_cr };
    size_t len[2] = { sizeof(junk) - 1u, sizeof(bare_cr) - 1u };
    char out[256];
    size_t n = 0;

    (void)state;
    assert_int_equal(uki_sbat_merge(in, len, 2, out, sizeof(out), &n), SGN_OK);
    out[n] = '\0';
    assert_string_equal(out,
        "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
        "one,1\ntwo,1\n");
}

static void test_uki_sbat_merge_reports_no_space(void **state)
{
    const char *in[1] = { STUB_SBAT };
    size_t len[1] = { strlen(STUB_SBAT) };
    char out[100];
    size_t n = 0;

    (void)state;
    assert_int_equal(uki_sbat_merge(in, len, 1, out, 10, &n), SGN_ERR_NOSPACE);
    assert_int_equal(uki_sbat_merge(in, len, 1, out, 90, &n), SGN_ERR_NOSPACE);
    assert_int_equal(uki_sbat_merge(in, len, 1, NULL, 90, &n),
                     SGN_ERR_INTERNAL);
}

/* ------------------------------------------------------------ cmdline */

static void test_uki_cmdline_collapses_whitespace(void **state)
{
    char out[64];

    (void)state;
    uki_cmdline_norm("  root=x \t quiet\n\nsplash  ", out);
    assert_string_equal(out, "root=x quiet splash");
    uki_cmdline_norm(" \x1c \v ", out);
    assert_string_equal(out, "");
    uki_cmdline_norm("one", out);
    assert_string_equal(out, "one");
}

/* ------------------------------------------------------------ layout */

static const char g_merged[] = "sbat,1,x\nuki,1,y\n";

static void test_uki_layout_places_sections(void **state)
{
    uki_part p[3] = {
        { ".osrel", 10, 0, 0, 0 },
        { ".linux", 0x1234, 1, 0, 0 },
        { ".initrd", 0x200, 0, 0, 0 },
    };
    uint64_t out_len = 0;
    const uint8_t *s;

    (void)state;
    assert_int_equal(uki_layout(g_stub, sizeof(g_stub), p, 3, g_merged,
                                sizeof(g_merged) - 1u, 0, &out_len), SGN_OK);

    /* File: after the stub, each at the next FileAlignment boundary. */
    assert_int_equal(p[0].raw_ptr, 0x800u);
    assert_int_equal(p[0].raw_size, 0x200u);
    assert_int_equal(p[1].raw_ptr, 0xA00u);
    assert_int_equal(p[1].raw_size, 0x1400u);
    assert_int_equal(p[2].raw_ptr, 0x1E00u);
    assert_int_equal(out_len, 0x2000u);

    /* Memory: after .sbat (0x2000 + its new size), SectionAlignment apart. */
    s = g_stub + TABLE + 2u * 40u;
    assert_memory_equal(s, ".osrel\0\0", 8);
    assert_int_equal(rd32(s + 12), 0x3000u);
    assert_int_equal(rd32(s + 8), 10u);
    assert_int_equal(rd32(s + 36), 0x40000040u);
    s += 40;
    assert_int_equal(rd32(s + 12), 0x4000u);
    assert_int_equal(rd32(s + 36), 0x40000020u);
    s += 40;
    assert_int_equal(rd32(s + 12), 0x6000u);

    assert_int_equal(g_stub[COFF + 2], 5);
    assert_int_equal(rd32(g_stub + OPT + 8), 0x100u + 10u + 0x1234u + 0x200u);
    assert_int_equal(rd32(g_stub + OPT + 56), 0x7000u);
    assert_int_equal(rd32(g_stub + OPT + 64), 0u);
    assert_int_equal(g_stub[OPT + 71] & 0x01, 0x01);     /* NX kept */

    /* .sbat rewritten where it stands, padded with zeroes. */
    assert_memory_equal(g_stub + 0x600, g_merged, sizeof(g_merged) - 1u);
    assert_int_equal(g_stub[0x600 + sizeof(g_merged) - 1u], 0);
    assert_int_equal(g_stub[0x600 + strlen(STUB_SBAT) - 1u], 0);
    assert_int_equal(rd32(g_stub + TABLE + 40u + 8u), sizeof(g_merged) - 1u);
}

static void test_uki_layout_clears_nx_for_a_kernel_without_it(void **state)
{
    uki_part p[1] = { { ".linux", 1, 1, 0, 0 } };
    uint64_t out_len = 0;

    (void)state;
    assert_int_equal(uki_layout(g_stub, sizeof(g_stub), p, 1, g_merged,
                                sizeof(g_merged) - 1u, 1, &out_len), SGN_OK);
    assert_int_equal(g_stub[OPT + 71] & 0x01, 0);
}

static void test_uki_layout_rounds_size_of_headers(void **state)
{
    uint64_t out_len = 0;

    (void)state;
    wr32(g_stub + OPT + 60, 0x3F0u);
    assert_int_equal(uki_layout(g_stub, sizeof(g_stub), NULL, 0, g_merged,
                                sizeof(g_merged) - 1u, 0, &out_len), SGN_OK);
    assert_int_equal(rd32(g_stub + OPT + 60), 0x400u);
    assert_int_equal(out_len, STUB_LEN);
}

static int layout_err(void)
{
    uki_part p[1] = { { ".osrel", 4, 0, 0, 0 } };
    uint64_t out_len = 0;

    return uki_layout(g_stub, sizeof(g_stub), p, 1, g_merged,
                      sizeof(g_merged) - 1u, 0, &out_len);
}

static void test_uki_layout_refuses_bad_stubs(void **state)
{
    (void)state;

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr16(g_stub + OPT, 0x10Bu);                         /* PE32 */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr32(g_stub + OPT + 112 + 4 * 8, 0x700u);           /* signed */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), "signed"));

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr32(g_stub + COFF + 8, 0x700u);                    /* symbol table */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr32(g_stub + OPT + 36, 0x300u);                    /* alignment */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr32(g_stub + TABLE + 20, 0x410u);                  /* unaligned raw */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr32(g_stub + TABLE + 40 + 16, 0x400u);             /* past the end */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr32(g_stub + OPT + 60, 0x600u);                    /* over .text */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);

    build_pe(g_stub, sizeof(g_stub), 1, 0);             /* no .sbat */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), ".sbat"));

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    wr16(g_stub + COFF + 2, 0u);                        /* no sections */
    assert_int_equal(layout_err(), SGN_ERR_FORMAT);
}

static void test_uki_layout_refuses_what_does_not_fit(void **state)
{
    char big[0x300];
    uki_part huge[1] = { { ".initrd", (uint64_t)1 << 32, 0, 0, 0 } };
    uki_part named[1] = { { ".toolongname", 1, 0, 0, 0 } };
    uki_part many[14];
    uint64_t out_len = 0;
    size_t i;

    (void)state;
    memset(big, 'a', sizeof(big));
    assert_int_equal(uki_layout(g_stub, sizeof(g_stub), NULL, 0, big,
                                sizeof(big), 0, &out_len), SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), ".sbat"));

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    assert_int_equal(uki_layout(g_stub, sizeof(g_stub), huge, 1, g_merged,
                                sizeof(g_merged) - 1u, 0, &out_len),
                     SGN_ERR_FORMAT);

    build_pe(g_stub, sizeof(g_stub), 1, 1);
    assert_int_equal(uki_layout(g_stub, sizeof(g_stub), named, 1, g_merged,
                                sizeof(g_merged) - 1u, 0, &out_len),
                     SGN_ERR_FORMAT);

    /* Room for 13 more headers before SizeOfHeaders; the 14th has none. */
    build_pe(g_stub, sizeof(g_stub), 1, 1);
    for (i = 0; i < 14; i++) {
        many[i].name = ".x";
        many[i].len = 1;
        many[i].code = 0;
    }
    assert_int_equal(uki_layout(g_stub, sizeof(g_stub), many, 14, g_merged,
                                sizeof(g_merged) - 1u, 0, &out_len),
                     SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), "no room"));

    assert_int_equal(uki_layout(NULL, 0, NULL, 0, g_merged, 1, 0, &out_len),
                     SGN_ERR_INTERNAL);
}

/* ------------------------------------------------------------ build */

static void read_back(const char *path, uint8_t **buf, size_t *len)
{
    char *b = NULL;

    assert_int_equal(sgn_read_file(path, &b, len, 1u << 20), SGN_OK);
    *buf = (uint8_t *)b;
}

static void expect_section(const uint8_t *img, size_t len, const char *name,
                           const void *want, size_t want_len)
{
    pe_hdr h;
    size_t off;
    size_t size;

    assert_int_equal(pe_parse(img, len, &h), PE_OK);
    assert_int_equal(pe_find_section(&h, img, len, name, &off, &size), PE_OK);
    assert_true(size >= want_len);
    assert_memory_equal(img + off, want, want_len);
}

static void make_inputs(char *stub, char *kernel, char *initrd, size_t cap)
{
    uint8_t s[STUB_LEN];
    uint8_t k[0x800];
    uint8_t rd[5000];
    size_t i;

    build_pe(s, sizeof(s), 1, 1);
    build_pe(k, sizeof(k), 0, 0);
    add_bzimage(k, "6.1.0-test (me@host) #1 SMP");
    for (i = 0; i < sizeof(rd); i++)
        rd[i] = (uint8_t)(i * 7u);
    put("stub.efi", s, sizeof(s), stub, cap);
    put("vmlinuz", k, sizeof(k), kernel, cap);
    put("initrd", rd, sizeof(rd), initrd, cap);
}

static void test_uki_build_end_to_end(void **state)
{
    char stub[256];
    char kernel[256];
    char initrd[256];
    char out[256];
    uki_spec spec = { 0 };
    uint8_t *img = NULL;
    uint8_t *rd = NULL;
    size_t len = 0;
    size_t rd_len = 0;

    (void)state;
    make_inputs(stub, kernel, initrd, sizeof(stub));
    (void)snprintf(out, sizeof(out), "%s/out.efi", g_dir);

    spec.stub = stub;
    spec.kernel = kernel;
    spec.initrd = initrd;
    spec.cmdline = "  root=live:x   quiet ";
    spec.osrel = UKI_BLESSED_OSREL;
    spec.out = out;
    assert_int_equal(uki_build(&spec), SGN_OK);

    read_back(out, &img, &len);
    read_back(initrd, &rd, &rd_len);
    expect_section(img, len, ".osrel", UKI_BLESSED_OSREL,
                   strlen(UKI_BLESSED_OSREL));
    expect_section(img, len, ".cmdline", "root=live:x quiet", 17);
    expect_section(img, len, ".uname", "6.1.0-test", 10);
    expect_section(img, len, ".initrd", rd, rd_len);
    expect_section(img, len, ".sbat", "sbat,1,", 7);
    /* The kernel is not NX_COMPAT, so the UKI is not either. */
    assert_int_equal(img[OPT + 71] & 0x01, 0);
    free(img);
    free(rd);

    /* No initrd, and a kernel with no version string: no .initrd, no .uname. */
    {
        uint8_t k[0x800];
        pe_hdr h;
        size_t off;
        size_t size;

        build_pe(k, sizeof(k), 1, 0);
        put("vmlinuz-plain", k, sizeof(k), kernel, sizeof(kernel));
        spec.initrd = NULL;
        assert_int_equal(uki_build(&spec), SGN_OK);
        read_back(out, &img, &len);
        assert_int_equal(pe_parse(img, len, &h), PE_OK);
        assert_int_equal(pe_find_section(&h, img, len, ".initrd", &off, &size),
                         PE_ERR_ABSENT);
        assert_int_equal(pe_find_section(&h, img, len, ".uname", &off, &size),
                         PE_ERR_ABSENT);
        assert_int_equal(img[OPT + 71] & 0x01, 0x01);
        free(img);
    }
}

/* A bzImage with no EFI stub, for falcon-bios's /uki (DESIGN Section
 * 13.10): refused as it always was, taken with bios_only, and a file that
 * is not a bzImage refused either way. */
static void test_uki_build_bios_only(void **state)
{
    char stub[256];
    char kernel[256];
    char initrd[256];
    char out[256];
    uint8_t k[0x800];
    uki_spec spec = { 0 };
    uint8_t *img = NULL;
    size_t len = 0;

    (void)state;
    make_inputs(stub, kernel, initrd, sizeof(stub));
    memset(k, 0, sizeof(k));
    add_bzimage(k, "6.6.0-devlink (me@host) #1");
    put("bzImage", k, sizeof(k), kernel, sizeof(kernel));
    (void)snprintf(out, sizeof(out), "%s/bios.efi", g_dir);

    spec.stub = stub;
    spec.kernel = kernel;
    spec.initrd = initrd;
    spec.cmdline = "";
    spec.osrel = "NAME=\"DevLink recovery\"\n";
    spec.out = out;
    assert_int_equal(uki_build(&spec), SGN_ERR_FORMAT);

    spec.bios_only = 1;
    assert_int_equal(uki_build(&spec), SGN_OK);
    read_back(out, &img, &len);
    expect_section(img, len, ".linux", k, sizeof(k));
    expect_section(img, len, ".uname", "6.6.0-devlink", 13);
    expect_section(img, len, ".osrel", "NAME=", 5);
    free(img);

    memset(k, 0, sizeof(k));
    put("notbz", k, sizeof(k), kernel, sizeof(kernel));
    assert_int_equal(uki_build(&spec), SGN_ERR_FORMAT);
}

static void test_uki_build_merges_the_kernel_sbat(void **state)
{
    static const char ksbat[] = "sbat,1,x\nlinux,1,L,linux,1,y\n";
    char stub[256];
    char kernel[256];
    char initrd[256];
    char out[256];
    uint8_t k[0x800];
    uki_spec spec = { 0 };
    uint8_t *img = NULL;
    size_t len = 0;

    (void)state;
    make_inputs(stub, kernel, initrd, sizeof(stub));
    build_pe(k, sizeof(k), 1, 0);
    wr16(k + COFF + 2, 2u);
    sect(k, 1, ".sbat", 0x2000u, sizeof(ksbat) - 1u, 0x600u, 0x200u);
    memcpy(k + 0x600, ksbat, sizeof(ksbat) - 1u);
    put("vmlinuz-sbat", k, sizeof(k), kernel, sizeof(kernel));
    (void)snprintf(out, sizeof(out), "%s/sbat.efi", g_dir);

    spec.stub = stub;
    spec.kernel = kernel;
    spec.initrd = NULL;
    spec.cmdline = "quiet";
    spec.osrel = "ID=x\n";
    spec.out = out;
    assert_int_equal(uki_build(&spec), SGN_OK);
    read_back(out, &img, &len);
    {
        static const char want[] =
            "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
            "stub,1,Test,stub,1,https://example.invalid\n"
            "linux,1,L,linux,1,y\n"
            UKI_SBAT_UKI_LINE "\n";

        expect_section(img, len, ".sbat", want, sizeof(want) - 1u);
    }
    free(img);
}

static void test_uki_build_refuses_bad_inputs(void **state)
{
    char stub[256];
    char kernel[256];
    char initrd[256];
    char junk[256];
    char out[256];
    uki_spec spec = { 0 };
    struct stat st;

    (void)state;
    make_inputs(stub, kernel, initrd, sizeof(stub));
    put("junk", "not a kernel at all", 19, junk, sizeof(junk));
    (void)snprintf(out, sizeof(out), "%s/bad.efi", g_dir);

    spec.stub = stub;
    spec.kernel = junk;
    spec.initrd = NULL;
    spec.cmdline = "quiet";
    spec.osrel = "ID=x\n";
    spec.out = out;
    assert_int_equal(uki_build(&spec), SGN_ERR_FORMAT);
    assert_non_null(strstr(sgn_error(), "too small"));

    {
        uint8_t zeros[0x800];

        memset(zeros, 0, sizeof(zeros));
        put("zeros", zeros, sizeof(zeros), junk, sizeof(junk));
        assert_int_equal(uki_build(&spec), SGN_ERR_FORMAT);
        assert_non_null(strstr(sgn_error(), "EFI-stub"));
        put("junk", "not a kernel at all", 19, junk, sizeof(junk));
    }

    spec.kernel = "/nonexistent/vmlinuz";
    assert_int_equal(uki_build(&spec), SGN_ERR_IO);

    spec.kernel = g_dir;                    /* a directory */
    assert_int_equal(uki_build(&spec), SGN_ERR_IO);

    spec.kernel = kernel;
    spec.initrd = "/nonexistent/initrd";
    assert_int_equal(uki_build(&spec), SGN_ERR_IO);

    spec.initrd = NULL;
    spec.stub = junk;                       /* not a PE */
    assert_int_equal(uki_build(&spec), SGN_ERR_FORMAT);

    spec.stub = stub;
    spec.out = "/nonexistent/out.efi";
    assert_int_equal(uki_build(&spec), SGN_ERR_IO);

    spec.out = NULL;
    assert_int_equal(uki_build(&spec), SGN_ERR_INTERNAL);
    assert_int_equal(uki_build(NULL), SGN_ERR_INTERNAL);

    /* Nothing is left behind by a failed build. */
    assert_int_not_equal(stat(out, &st), 0);
}

/* ------------------------------------------------------------ uki verb */

static void test_uki_main_arguments(void **state)
{
    char stub[256];
    char kernel[256];
    char initrd[256];
    char osrel[256];
    char nul[256];
    char out[256];
    struct stat st;

    (void)state;
    make_inputs(stub, kernel, initrd, sizeof(stub));
    put("osrel", "ID=x\n", 5, osrel, sizeof(osrel));
    put("nul", "ID=\0x\n", 6, nul, sizeof(nul));
    (void)snprintf(out, sizeof(out), "%s/verb.efi", g_dir);

    {
        char *argv[] = { (char *)"uki", (char *)"--linux", kernel,
                         (char *)"--initrd", initrd, (char *)"--cmdline",
                         (char *)"quiet", (char *)"--os-release", osrel,
                         (char *)"--stub", stub, (char *)"--output", out,
                         NULL };

        assert_int_equal(sgn_uki_main(13, argv), SGN_OK);
        assert_int_equal(stat(out, &st), 0);
    }
    {
        char *argv[] = { (char *)"uki", (char *)"--linux", kernel, NULL };

        assert_int_equal(sgn_uki_main(3, argv), SGN_ERR_ARG);
    }
    {
        char *argv[] = { (char *)"uki", (char *)"--bogus", kernel, NULL };

        assert_int_equal(sgn_uki_main(3, argv), SGN_ERR_ARG);
    }
    {
        char *argv[] = { (char *)"uki", (char *)"--linux", NULL };

        assert_int_equal(sgn_uki_main(2, argv), SGN_ERR_ARG);
    }
    {
        char *argv[] = { (char *)"uki", (char *)"--linux", kernel,
                         (char *)"--cmdline", (char *)"quiet",
                         (char *)"--os-release", nul, (char *)"--stub", stub,
                         (char *)"--output", out, NULL };

        assert_int_equal(sgn_uki_main(11, argv), SGN_ERR_FORMAT);
    }
    {
        char *argv[] = { (char *)"uki", (char *)"--help", NULL };

        assert_int_equal(sgn_uki_main(2, argv), SGN_OK);
    }
}

/* ------------------------------------------------------------------ run */

int run_uki_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_uki_uname_reads_the_version),
        cmocka_unit_test(test_uki_uname_needs_the_whole_shape),
        cmocka_unit_test(test_uki_uname_refuses_what_is_not_a_bzimage),
        cmocka_unit_test(test_uki_sbat_merge_in_order),
        cmocka_unit_test(test_uki_sbat_merge_skips_what_is_not_sbat),
        cmocka_unit_test(test_uki_sbat_merge_reports_no_space),
        cmocka_unit_test(test_uki_cmdline_collapses_whitespace),
        cmocka_unit_test_setup(test_uki_layout_places_sections, setup_stub),
        cmocka_unit_test_setup(test_uki_layout_clears_nx_for_a_kernel_without_it,
                               setup_stub),
        cmocka_unit_test_setup(test_uki_layout_rounds_size_of_headers,
                               setup_stub),
        cmocka_unit_test_setup(test_uki_layout_refuses_bad_stubs, setup_stub),
        cmocka_unit_test_setup(test_uki_layout_refuses_what_does_not_fit,
                               setup_stub),
        cmocka_unit_test(test_uki_build_end_to_end),
        cmocka_unit_test(test_uki_build_bios_only),
        cmocka_unit_test(test_uki_build_merges_the_kernel_sbat),
        cmocka_unit_test(test_uki_build_refuses_bad_inputs),
        cmocka_unit_test(test_uki_main_arguments),
    };

    return cmocka_run_group_tests_name("UKI builder (bless tool)", tests,
                                       setup_dir, teardown_dir);
}
