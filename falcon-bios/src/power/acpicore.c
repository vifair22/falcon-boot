/* falcon-bios: what ACPI says a power-off is. See acpicore.h. */

#include <power/acpicore.h>

#include <lib/string.h>

#define HDR_LEN      36u
#define FADT_MIN     76u     /* up to and including PM1b_CNT_BLK */
#define S5_WINDOW    512u
#define S5_OVERLAP   32u     /* longer than a name and a package head */
#define TABLE_MAX    (1024u * 1024u)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint8_t sum(const uint8_t *p, size_t n)
{
    uint8_t s = 0;
    size_t i;

    for (i = 0; i < n; i++)
        s = (uint8_t)(s + p[i]);
    return s;
}

/* A table's checksum covers its whole length, read in pieces. */
static int table_ok(acpi_read_fn rd, void *ctx, uint32_t addr, uint32_t len)
{
    uint8_t buf[256];
    uint8_t s = 0;
    uint32_t off = 0;

    if (len < HDR_LEN || len > TABLE_MAX || addr > 0xFFFFFFFFu - len)
        return 0;
    while (off < len) {
        uint32_t k = len - off < sizeof buf ? len - off : (uint32_t)sizeof buf;

        if (rd(ctx, addr + off, buf, k) != 0)
            return 0;
        s = (uint8_t)(s + sum(buf, k));
        off += k;
    }
    return s == 0;
}

/* The RSDP in [base, base + len), on 16-byte boundaries. */
static int rsdp_in(acpi_read_fn rd, void *ctx, uint32_t base, uint32_t len,
                   uint8_t rsdp[36])
{
    uint32_t at;

    for (at = 0; at + 20u <= len; at += 16u) {
        if (rd(ctx, base + at, rsdp, 20) != 0)
            continue;
        if (memcmp(rsdp, "RSD PTR ", 8) != 0 || sum(rsdp, 20) != 0)
            continue;
        /* ACPI 2.0 and later: the extended checksum over 36 bytes too. */
        if (rsdp[15] >= 2u) {
            if (at + 36u > len || rd(ctx, base + at, rsdp, 36) != 0 ||
                sum(rsdp, 36) != 0)
                continue;
        }
        return 1;
    }
    return 0;
}

/* The FADT's address from the RSDT (or an XSDT whose pointers fit). */
static int find_fadt(acpi_read_fn rd, void *ctx, const uint8_t rsdp[36],
                     uint32_t *fadt)
{
    uint8_t hdr[HDR_LEN];
    uint32_t sdt = le32(rsdp + 16);
    uint32_t width = 4;
    uint32_t len;
    uint32_t i;

    if (rsdp[15] >= 2u && le32(rsdp + 28) == 0 && le32(rsdp + 24) != 0) {
        sdt = le32(rsdp + 24);
        width = 8;
    }
    if (sdt == 0 || rd(ctx, sdt, hdr, HDR_LEN) != 0)
        return ACPI_NO_FADT;
    len = le32(hdr + 4);
    if (!table_ok(rd, ctx, sdt, len))
        return ACPI_BAD_TABLE;
    for (i = HDR_LEN; i + width <= len; i += width) {
        uint8_t e[8];
        uint8_t sig[4];
        uint32_t at;

        if (rd(ctx, sdt + i, e, width) != 0)
            return ACPI_BAD_TABLE;
        if (width == 8 && le32(e + 4) != 0)
            continue;
        at = le32(e);
        if (at == 0 || rd(ctx, at, sig, 4) != 0)
            continue;
        if (memcmp(sig, "FACP", 4) == 0) {
            *fadt = at;
            return ACPI_OK;
        }
    }
    return ACPI_NO_FADT;
}

/* An AML integer constant at p: ZeroOp, OneOp, BytePrefix, WordPrefix,
 * DWordPrefix, OnesOp. Its low byte, and how many bytes it took. */
static size_t aml_int(const uint8_t *p, size_t left, uint8_t *v)
{
    if (left == 0)
        return 0;
    switch (p[0]) {
    case 0x00: *v = 0; return 1;
    case 0x01: *v = 1; return 1;
    case 0xFF: *v = 0xFF; return 1;
    case 0x0A: if (left < 2) return 0; *v = p[1]; return 2;
    case 0x0B: if (left < 3) return 0; *v = p[1]; return 3;
    case 0x0C: if (left < 5) return 0; *v = p[1]; return 5;
    default:   return 0;
    }
}

int acpi_parse_s5(const uint8_t *aml, size_t len, uint8_t *typa,
                  uint8_t *typb)
{
    size_t i;

    if (aml == NULL || typa == NULL || typb == NULL)
        return ACPI_NO_S5;
    for (i = 1; i + 4u < len; i++) {
        size_t at;
        size_t k;
        uint8_t lead;
        uint8_t num;

        if (memcmp(aml + i, "_S5_", 4) != 0)
            continue;
        /* NameOp before the name, or NameOp then the root prefix. */
        if (!(aml[i - 1u] == 0x08 ||
              (i >= 2u && aml[i - 1u] == 0x5C && aml[i - 2u] == 0x08)))
            continue;
        at = i + 4u;
        if (at >= len || aml[at] != 0x12)       /* PackageOp */
            continue;
        at++;
        if (at >= len)
            continue;
        lead = aml[at];
        at += 1u + (size_t)(lead >> 6);         /* PkgLength */
        if (at >= len)
            continue;
        num = aml[at++];                        /* NumElements */
        if (num == 0 || at >= len)
            continue;
        k = aml_int(aml + at, len - at, typa);
        if (k == 0)
            continue;
        at += k;
        /* A package cut short by the end of what was read is looked for
         * again in the next window, not taken as one of a single value. */
        if (num >= 2u) {
            if (aml_int(aml + at, len - at, typb) == 0)
                continue;
        } else {
            *typb = *typa;
        }
        return ACPI_OK;
    }
    return ACPI_NO_S5;
}

/* \_S5 in the DSDT, read a window at a time with an overlap, so a name
 * across a window's edge is still found. */
static int dsdt_s5(acpi_read_fn rd, void *ctx, uint32_t dsdt, acpi_s5 *out)
{
    uint8_t hdr[HDR_LEN];
    uint8_t win[S5_WINDOW];
    uint32_t len;
    uint32_t off;

    if (dsdt == 0 || rd(ctx, dsdt, hdr, HDR_LEN) != 0 ||
        memcmp(hdr, "DSDT", 4) != 0)
        return ACPI_NO_S5;
    len = le32(hdr + 4);
    if (!table_ok(rd, ctx, dsdt, len))
        return ACPI_BAD_TABLE;
    for (off = HDR_LEN; off < len; off += S5_WINDOW - S5_OVERLAP) {
        uint32_t k = len - off < S5_WINDOW ? len - off : S5_WINDOW;

        if (rd(ctx, dsdt + off, win, k) != 0)
            return ACPI_BAD_TABLE;
        if (acpi_parse_s5(win, k, &out->slp_typa, &out->slp_typb) ==
            ACPI_OK)
            return ACPI_OK;
        if (k < S5_WINDOW)
            break;
    }
    return ACPI_NO_S5;
}

int acpi_find_s5(acpi_read_fn rd, void *ctx, uint32_t ebda, acpi_s5 *out)
{
    uint8_t rsdp[36];
    uint8_t f[148];
    uint32_t fadt = 0;
    uint32_t len;
    uint32_t dsdt;
    int rc;

    if (rd == NULL || out == NULL)
        return ACPI_NO_RSDP;
    memset(out, 0, sizeof *out);
    if (!(ebda != 0 && rsdp_in(rd, ctx, ebda, 1024, rsdp)) &&
        !rsdp_in(rd, ctx, 0xE0000u, 0x20000u, rsdp))
        return ACPI_NO_RSDP;
    rc = find_fadt(rd, ctx, rsdp, &fadt);
    if (rc != ACPI_OK)
        return rc;
    if (rd(ctx, fadt, f, HDR_LEN) != 0)
        return ACPI_BAD_TABLE;
    len = le32(f + 4);
    if (len < FADT_MIN || !table_ok(rd, ctx, fadt, len))
        return ACPI_BAD_TABLE;
    if (rd(ctx, fadt, f, len < sizeof f ? len : (uint32_t)sizeof f) != 0)
        return ACPI_BAD_TABLE;

    out->smi_cmd = le32(f + 48);
    out->acpi_enable = f[52];
    out->pm1a_cnt = le32(f + 64);
    out->pm1b_cnt = le32(f + 68);
    if (out->pm1a_cnt == 0 || out->pm1a_cnt > 0xFFFFu ||
        out->pm1b_cnt > 0xFFFFu)
        return ACPI_NO_FADT;
    dsdt = le32(f + 40);
    /* X_DSDT, when the table is long enough to have one and it fits. */
    if (len >= 148u && le32(f + 144) == 0 && le32(f + 140) != 0)
        dsdt = le32(f + 140);
    return dsdt_s5(rd, ctx, dsdt, out);
}

uint16_t acpi_s5_word(uint8_t slp_typ)
{
    return (uint16_t)(((unsigned)(slp_typ & 7u) << 10) | 0x2000u);
}
