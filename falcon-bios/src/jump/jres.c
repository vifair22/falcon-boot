/* falcon jump: installing the reverse thunk. See jres.h and rthunk.S. */

#include <start/bios.h>
#include <jump/jres.h>
#include <mem/lowmem.h>

#include <lib/string.h>

#define IVT_INT13 (0x13u * 4u)
#define IVT_INT15 (0x15u * 4u)
#define IVT_INT16 (0x16u * 4u)
#define IVT_INT18 (0x18u * 4u)
#define IVT_INT19 (0x19u * 4u)

/* The blob, copied, never called where it is linked (stage 3 is above a
 * megabyte, out of real mode's reach). Its length through integers, as
 * vcd.c takes vcd_blob's. */
extern const unsigned char jres_blob_start[];
extern const unsigned char jres_blob_end[];

#define BDA_NDISKS 0x475u

static uint16_t g_seg;
static e820_entry g_carved[JRES_E820_MAX];
static uint32_t g_kbd[JRES_KBD_MAX];
static unsigned g_nkbd;

void jres_keyboards(const uint32_t *entries, unsigned n)
{
    unsigned i;

    g_nkbd = 0;
    for (i = 0; entries != (const uint32_t *)0 && i < n &&
                i < JRES_KBD_MAX; i++)
        g_kbd[g_nkbd++] = entries[i];
}

static void put32(uint8_t *at, uint32_t v)
{
    memcpy(at, &v, 4);
}

/* The INT 15h half's table and ceilings: the BIOS's map with the block and
 * the caller's ranges taken out. */
static int carve(uint8_t *at, uint32_t b, const e820_range *keep,
                 unsigned n_keep)
{
    e820_range all[E820_CARVE_MAX];
    const e820_entry *first = e820_at(0);
    unsigned n = 0;
    unsigned out = 0;
    unsigned i;
    uint16_t v;

    if (first == (const e820_entry *)0 || n_keep + 1u > E820_CARVE_MAX)
        return -1;
    all[n].base = b;
    all[n].len = JRES_BYTES;
    n++;
    for (i = 0; i < n_keep; i++)
        all[n++] = keep[i];
    if (e820_carve(first, e820_count(), all, n, g_carved, JRES_E820_MAX,
                   &out) != 0)
        return -1;
    for (i = 0; i < out; i++) {
        uint8_t *m = at + JRES_OFF_E820 + i * 20u;

        memcpy(m, &g_carved[i].base, 8);
        memcpy(m + 8, &g_carved[i].len, 8);
        memcpy(m + 16, &g_carved[i].type, 4);
    }
    v = (uint16_t)out;
    memcpy(at + JRES_OFF_NE820, &v, 2);
    v = e820_e801_cap_low(all, n);
    memcpy(at + JRES_OFF_E801LOW, &v, 2);
    v = e820_e801_cap(all, n);
    memcpy(at + JRES_OFF_E801CAP, &v, 2);
    return 0;
}

/* `hook13` 0 is the keyboard block: INT 13h left alone, DRIVE 0. */
static int install(int hook13, uint8_t drive, uint32_t entry,
                   uint32_t stack_top, const e820_range *keep,
                   unsigned n_keep, uint32_t *base)
{
    size_t len = (size_t)((uintptr_t)jres_blob_end -
                          (uintptr_t)jres_blob_start);
    uint16_t seg = lowmem_segment(JRES_BYTES);
    uint32_t b;
    uint32_t v;
    uint16_t kb;
    uint8_t *at;
    uint8_t *desc;
    unsigned i;

    if (seg == 0 || len > JRES_BYTES || (!hook13 && g_nkbd == 0))
        return -1;
    b = (uint32_t)seg * 16u;
    at = (uint8_t *)(uintptr_t)b;
    memcpy(at, jres_blob_start, len);

    if (hook13)
        put32(at + JRES_OFF_OLDINT13, peek32(IVT_INT13));
    at[JRES_OFF_DRIVE] = hook13 ? drive : 0;
    put32(at + JRES_OFF_GDTR + 2u, b + JRES_OFF_GDT);
    put32(at + JRES_OFF_ENTRY, entry);
    put32(at + JRES_OFF_STACK, stack_top);
    /* The far pointers: the assembly wrote the offsets, relative to the
     * block; the 32-bit one needs the block's linear address added, the
     * real-mode one its segment. */
    memcpy(&v, at + JRES_OFF_PM32PTR, 4);
    put32(at + JRES_OFF_PM32PTR, v + b);
    at[JRES_OFF_RMPTR + 2u] = (uint8_t)(seg & 0xFFu);
    at[JRES_OFF_RMPTR + 3u] = (uint8_t)(seg >> 8);
    /* The 16-bit code descriptor's base is the block, so the 16-bit code
     * that turns protection off runs at the offsets it was assembled for. */
    desc = at + JRES_OFF_GDT + 0x18u;
    desc[2] = (uint8_t)(b & 0xFFu);
    desc[3] = (uint8_t)((b >> 8) & 0xFFu);
    desc[4] = (uint8_t)((b >> 16) & 0xFFu);
    desc[7] = (uint8_t)(b >> 24);

    if (keep != (const e820_range *)0) {
        if (carve(at, b, keep, n_keep) != 0)
            return -1;
        put32(at + JRES_OFF_OLDINT15, peek32(IVT_INT15));
    }

    /* The release record: everything outside the block this changes, and
     * the disk count as it was before the route raises it. */
    put32(at + JRES_OFF_OLDINT18, peek32(IVT_INT18));
    put32(at + JRES_OFF_OLDINT19, peek32(IVT_INT19));
    kb = peek16(BDA_MEMSIZE);
    memcpy(at + JRES_OFF_OLDKB, &kb, 2);
    at[JRES_OFF_OLDNDISK] = peek8(BDA_NDISKS);
    at[JRES_OFF_NKBD] = (uint8_t)g_nkbd;
    for (i = 0; i < g_nkbd; i++)
        put32(at + JRES_OFF_KBD + i * 4u, g_kbd[i]);
    if (g_nkbd != 0)
        put32(at + JRES_OFF_OLDINT16, peek32(IVT_INT16));

    lowmem_take(seg);
    if (hook13)
        poke32(IVT_INT13, ((uint32_t)seg << 16) | (uint32_t)JRES_OFF_CODE);
    if (keep != (const e820_range *)0)
        poke32(IVT_INT15, ((uint32_t)seg << 16) | (uint32_t)JRES_OFF_CODE15);
    /* No chaining: both end in a cold reset (rthunk.S, jres_reset). */
    poke32(IVT_INT18, ((uint32_t)seg << 16) | (uint32_t)JRES_OFF_RESET);
    poke32(IVT_INT19, ((uint32_t)seg << 16) | (uint32_t)JRES_OFF_RESET);
    if (g_nkbd != 0)
        poke32(IVT_INT16, ((uint32_t)seg << 16) | (uint32_t)JRES_OFF_CODE16);
    g_seg = seg;
    if (base != (uint32_t *)0)
        *base = b;
    if (hook13) {
        bios_puts("falcon-bios: reverse thunk for drive ");
        bios_put_hex64(drive);
    } else {
        bios_puts("falcon-bios: reverse thunk for no drive");
    }
    bios_puts(" at ");
    bios_put_hex64(b);
    if (g_nkbd != 0) {
        bios_puts(", INT 16h for ");
        bios_put_dec(g_nkbd);
        bios_puts(g_nkbd == 1u ? " module's keyboards" : " modules' keyboards");
    }
    bios_puts("\n");
    g_nkbd = 0;
    return 0;
}

int jres_install(uint8_t drive, uint32_t entry, uint32_t stack_top,
                 const e820_range *keep, unsigned n_keep, uint32_t *base)
{
    return install(1, drive, entry, stack_top, keep, n_keep, base);
}

int jres_install_keyboards(uint32_t stack_top, const e820_range *keep,
                           unsigned n_keep)
{
    return install(0, 0, 0, stack_top, keep, n_keep, (uint32_t *)0);
}

/* Puts back what the block at `seg` changed outside itself, from its
 * release record: each vector that still points into the block, the BDA's
 * memory count when it still ends where the block starts, and the disk
 * count when it is still the one the route raised. What something else
 * changed since is left alone. */
static void release(uint16_t seg)
{
    uint32_t b = (uint32_t)seg * 16u;
    uint32_t hook = (uint32_t)seg << 16;
    uint8_t ndisk = peek8(b + JRES_OFF_OLDNDISK);

    if (peek32(IVT_INT13) == (hook | JRES_OFF_CODE))
        poke32(IVT_INT13, peek32(b + JRES_OFF_OLDINT13));
    if (peek16(b + JRES_OFF_NE820) != 0 &&
        peek32(IVT_INT15) == (hook | JRES_OFF_CODE15))
        poke32(IVT_INT15, peek32(b + JRES_OFF_OLDINT15));
    if (peek32(IVT_INT18) == (hook | JRES_OFF_RESET))
        poke32(IVT_INT18, peek32(b + JRES_OFF_OLDINT18));
    if (peek32(IVT_INT19) == (hook | JRES_OFF_RESET))
        poke32(IVT_INT19, peek32(b + JRES_OFF_OLDINT19));
    if (peek8(b + JRES_OFF_NKBD) != 0 &&
        peek32(IVT_INT16) == (hook | JRES_OFF_CODE16))
        poke32(IVT_INT16, peek32(b + JRES_OFF_OLDINT16));
    if (peek16(BDA_MEMSIZE) == (uint16_t)(b / 1024u))
        poke16(BDA_MEMSIZE, peek16(b + JRES_OFF_OLDKB));
    if (peek8(BDA_NDISKS) == (uint8_t)(ndisk + 1u) &&
        peek8(b + JRES_OFF_DRIVE) == (uint8_t)(0x80u + ndisk))
        poke8(BDA_NDISKS, ndisk);
}

void jres_remove(void)
{
    if (g_seg == 0)
        return;
    release(g_seg);
    g_seg = 0;
}

/* The segment of the falcon jump block the vector at `ivt` leads to, as
 * its entry at offset `off`, or 0. */
static uint16_t behind(uint32_t ivt, uint32_t off)
{
    uint32_t v = peek32(ivt);
    uint16_t seg = (uint16_t)(v >> 16);
    uint32_t b = (uint32_t)seg * 16u;

    if ((v & 0xFFFFu) != off || b == 0 || b + JRES_BYTES > 0xA0000u ||
        peek32(b + JRES_OFF_MAGIC) != JRES_MAGIC)
        return 0;
    return seg;
}

/* The block INT 13h leads to; else the keyboard block, which hooks no INT
 * 13h, by INT 16h, or by INT 15h should something have hooked INT 16h
 * since. */
static uint16_t found(void)
{
    uint16_t seg = behind(IVT_INT13, JRES_OFF_CODE);

    if (seg == 0)
        seg = behind(IVT_INT16, JRES_OFF_CODE16);
    if (seg == 0)
        seg = behind(IVT_INT15, JRES_OFF_CODE15);
    return seg;
}

uint8_t jres_drive(void)
{
    uint16_t seg = found();

    return seg != 0 ? peek8((uint32_t)seg * 16u + JRES_OFF_DRIVE) : 0;
}

uint8_t jres_release(void)
{
    uint16_t seg = found();
    uint8_t drive;

    if (seg == 0)
        return 0;
    drive = peek8((uint32_t)seg * 16u + JRES_OFF_DRIVE);
    release(seg);
    if (seg == g_seg)
        g_seg = 0;
    if (drive != 0) {
        bios_puts("falcon-bios: released falcon jump's resident for drive ");
        bios_put_hex64(drive);
    } else {
        bios_puts("falcon-bios: released falcon jump's keyboard resident");
    }
    bios_puts(" at ");
    bios_put_hex64((uint64_t)seg * 16u);
    bios_puts("\n");
    return drive;
}
