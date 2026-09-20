/* falcon-bios: booting a Linux kernel directly
 * (docs/notes/bios-route.md Section 4.2).
 *
 * The 64-bit entry of the x86 boot protocol, which is the one route into a
 * kernel that does not require going back to real mode. The loader is already
 * in long mode with paging on, which is exactly the state the protocol asks
 * for, so the handoff is: read the setup header out of the image, load the
 * protected-mode half somewhere it will accept, fill in a zero page, and
 * jump.
 *
 * WHAT THE PROTOCOL REQUIRES AT THE JUMP, and every one of these is a way to
 * fail silently if it is wrong:
 *
 *   - 64-bit mode, paging on, with the kernel, the initrd, the zero page and
 *     the command line all inside the identity map. Ours is a gigabyte
 *     (biosdefs.h IDENTITY_MAP), which bounds where any of them may go.
 *   - interrupts off, and no interrupt handler expected to survive.
 *   - a GDT whose code selector is 0x10 and data selector 0x18. Ours is not:
 *     falcon-bios puts code64 at 0x08 because nothing had an opinion until
 *     now. So a second, handoff GDT is built for the jump.
 *   - %rsi holding the zero page.
 *
 * WHAT THE ZERO PAGE IS. The first 0x1F1 bytes are fields the boot loader
 * fills in and the kernel reads; from 0x1F1 to the end of the setup header
 * is a copy of the header out of the kernel image itself, with a handful of
 * fields written back. It is not a structure this file declares, because
 * declaring it would mean declaring every field of a layout that has grown
 * for thirty years; the ones that matter are named offsets, which is how the
 * kernel's own documentation describes them.
 */

#include <bios.h>
#include <linux.h>
#include <e820.h>

#include <lib/string.h>

/* Setup header offsets, from the kernel's Documentation/arch/x86/boot.rst.
 * Offsets into the boot sector, which is also where they sit in the zero
 * page: the header is copied across at the same address. */
#define HDR_SETUP_SECTS     0x1F1   /* u8   512-byte sectors of setup code */
#define HDR_SYSSIZE         0x1F4   /* u32  16-byte paras of the pm kernel */
#define HDR_BOOT_FLAG       0x1FE   /* u16  0xAA55 */
#define HDR_HEADER          0x202   /* u32  "HdrS" */
#define HDR_VERSION         0x206   /* u16  boot protocol version */
#define HDR_TYPE_OF_LOADER  0x210   /* u8 */
#define HDR_CODE32_START    0x214   /* u32  where the pm kernel was loaded */
#define HDR_LOADFLAGS       0x211   /* u8 */
#define HDR_RAMDISK_IMAGE   0x218   /* u32 */
#define HDR_RAMDISK_SIZE    0x21C   /* u32 */
#define HDR_HEAP_END_PTR    0x224   /* u16 */
#define HDR_CMD_LINE_PTR    0x228   /* u32 */
#define HDR_INITRD_ADDR_MAX 0x22C   /* u32 */
#define HDR_KERNEL_ALIGNMENT 0x230  /* u32 */
#define HDR_RELOCATABLE     0x234   /* u8 */
#define HDR_MIN_ALIGNMENT   0x235   /* u8   log2 */
#define HDR_CMDLINE_SIZE    0x238   /* u32 */
#define HDR_XLOADFLAGS      0x236   /* u16 */
#define HDR_PREF_ADDRESS    0x258   /* u64 */
#define HDR_INIT_SIZE       0x260   /* u32  memory the kernel needs to run */

/* Zero page fields that are not part of the setup header. */
#define BP_E820_ENTRIES     0x1E8   /* u8 */
#define BP_E820_TABLE       0x2D0   /* E820_MAX * 20 bytes */

/* The second byte of the two-byte jump at 0x200 is the end of the setup
 * header, relative to 0x202. It is how the header says how long it is, and
 * the only way to copy all of it without knowing which version this is. */
#define HDR_JUMP_LEN        0x201   /* u8 */

#define HDRS_MAGIC          0x53726448u   /* "HdrS" */
#define XLF_KERNEL_64       0x0001u       /* the 64-bit entry exists */
#define LOADFLAG_LOADED_HIGH 0x01u
#define LOADFLAG_CAN_USE_HEAP 0x80u
#define LOADER_TYPE_UNKNOWN 0xFFu         /* no id assigned to falcon-boot */

/* The entry point sits this far into the loaded protected-mode kernel. */
#define ENTRY64_OFFSET      0x200

/* One boot sector plus the setup sectors, read before anything else so the
 * header can be consulted. The protocol caps setup at 64 sectors. */
static uint8_t g_setup[65u * 512u];

/* The handoff GDT. Selector 0x10 is code, 0x18 is data, because that is what
 * the kernel's entry assumes; the null and unused entries are there to put
 * them at those indices and nothing else. */
static uint64_t g_gdt[4] __attribute__((aligned(16))) = {
    0,
    0,
    0x00AF9A000000FFFFu,   /* 0x10: code64, L=1 */
    0x00CF92000000FFFFu    /* 0x18: data64 */
};

static struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} g_gdtr;

static uint8_t rd8(const uint8_t *p, unsigned off)
{
    return p[off];
}

static uint16_t rd16(const uint8_t *p, unsigned off)
{
    return (uint16_t)((uint16_t)p[off] | ((uint16_t)p[off + 1u] << 8));
}

static uint32_t rd32(const uint8_t *p, unsigned off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) |
           ((uint32_t)p[off + 2u] << 16) | ((uint32_t)p[off + 3u] << 24);
}

static uint64_t rd64(const uint8_t *p, unsigned off)
{
    return (uint64_t)rd32(p, off) | ((uint64_t)rd32(p, off + 4u) << 32);
}

static void wr8(uint8_t *p, unsigned off, uint8_t v)
{
    p[off] = v;
}

static void wr16(uint8_t *p, unsigned off, uint16_t v)
{
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1u] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, unsigned off, uint32_t v)
{
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1u] = (uint8_t)((v >> 8) & 0xFFu);
    p[off + 2u] = (uint8_t)((v >> 16) & 0xFFu);
    p[off + 3u] = (uint8_t)((v >> 24) & 0xFFu);
}

static void say(const char *s)
{
    bios_puts("falcon-bios: ");
    bios_puts(s);
    bios_puts("\n");
}

/* Copies the E820 map into the zero page, in the 20-byte form the protocol
 * uses there: the ACPI attributes word is not part of it. */
static void fill_e820(uint8_t *zp)
{
    unsigned n = e820_read();
    unsigned i;

    if (n > E820_MAX)
        n = E820_MAX;
    for (i = 0; i < n; i++) {
        const e820_entry *e = e820_at(i);
        unsigned at = BP_E820_TABLE + i * 20u;

        if (e == (const e820_entry *)0)
            break;
        wr32(zp, at, (uint32_t)(e->base & 0xFFFFFFFFu));
        wr32(zp, at + 4u, (uint32_t)(e->base >> 32));
        wr32(zp, at + 8u, (uint32_t)(e->len & 0xFFFFFFFFu));
        wr32(zp, at + 12u, (uint32_t)(e->len >> 32));
        wr32(zp, at + 16u, e->type);
    }
    wr8(zp, BP_E820_ENTRIES, (uint8_t)i);
}

/* Reads an object into `dst` in bounce-sized pieces. The read callback is
 * whatever the caller had open, and may be an ISO9660 file inside an exFAT
 * file inside a partition; each layer is a callback and none of them knows. */
static int load_object(const linux_object *o, uint64_t from, uint64_t len,
                       uint64_t dst)
{
    uint64_t done = 0;

    if (from > o->size || len > o->size - from)
        return -1;
    while (done < len) {
        uint64_t chunk = len - done;

        if (chunk > 65536u)
            chunk = 65536u;
        if (o->read(o->ctx, from + done, chunk,
                    (void *)(uintptr_t)(dst + done)) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

/* Never returns. Everything the kernel needs is in memory; this is the
 * handoff itself, and it is the fifth and last of Section 5.3's assembly
 * jobs, written here as inline asm because it is six instructions and
 * separating them from the values they use would help nobody. */
static void enter_kernel(uint64_t entry, uint64_t zeropage)
{
    g_gdtr.limit = (uint16_t)(sizeof g_gdt - 1u);
    g_gdtr.base = (uint64_t)(uintptr_t)g_gdt;

    __asm__ __volatile__(
        "cli\n\t"
        "lgdt   %[gdtr]\n\t"
        /* The data selectors first: a far return reloads CS, and after it
         * nothing here runs again. */
        "movw   $0x18, %%ax\n\t"
        "movw   %%ax, %%ds\n\t"
        "movw   %%ax, %%es\n\t"
        "movw   %%ax, %%ss\n\t"
        "movw   %%ax, %%fs\n\t"
        "movw   %%ax, %%gs\n\t"
        /* A far return is the only way to load CS in 64-bit mode short of
         * an interrupt return; push the selector and the target and take
         * it. RSI carries the zero page, which is the whole ABI. */
        "pushq  $0x10\n\t"
        "pushq  %[entry]\n\t"
        "lretq\n\t"
        :
        : [gdtr] "m"(g_gdtr), [entry] "r"(entry), "S"(zeropage)
        : "rax", "memory");
    for (;;)
        __asm__ __volatile__("hlt");
}

int linux_boot(const linux_object *kernel, const linux_object *initrd,
               unsigned n_initrd, const char *cmdline)
{
    uint8_t *zp = (uint8_t *)(uintptr_t)ZEROPAGE_ADDR;
    char *cmd = (char *)(uintptr_t)CMDLINE_ADDR;
    uint64_t setup_len;
    uint64_t pm_len;
    uint64_t load_at = KERNEL_ADDR;
    uint64_t align;
    uint64_t init_size;
    uint64_t initrd_at = 0;
    uint64_t initrd_len = 0;
    uint64_t initrd_max;
    uint32_t cmdline_cap;
    size_t cmdlen;
    unsigned i;
    uint16_t version;

    if (kernel == (const linux_object *)0 || kernel->size < 1024u) {
        say("that is not a kernel");
        return -1;
    }

    /* The first two sectors, not one: setup_sects is at 0x1F1, inside the
     * boot sector, but the "HdrS" magic that says the header is a header is
     * at 0x202, which is past it. The setup header straddles the end of the
     * boot sector and has since the field was added. */
    if (kernel->read(kernel->ctx, 0, 1024, g_setup) != 0) {
        say("the kernel's first sectors would not read");
        return -1;
    }
    if (rd16(g_setup, HDR_BOOT_FLAG) != 0xAA55u ||
        rd32(g_setup, HDR_HEADER) != HDRS_MAGIC) {
        say("the kernel has no setup header; it is not a bzImage");
        return -1;
    }
    version = rd16(g_setup, HDR_VERSION);
    /* 2.12 is where xloadflags and init_size are both present and settled.
     * Anything this loader will meet on a distribution ISO is well past it;
     * refusing is better than guessing at a header that is not there. */
    if (version < 0x020Cu) {
        say("the kernel's boot protocol is older than this loader supports");
        return -1;
    }
    /* The kernel says whether it has the entry this route takes. A 32-bit
     * kernel does not, and there is no second route here. */
    if ((rd16(g_setup, HDR_XLOADFLAGS) & XLF_KERNEL_64) == 0) {
        say("the kernel has no 64-bit entry point");
        return -1;
    }

    setup_len = rd8(g_setup, HDR_SETUP_SECTS);
    if (setup_len == 0)
        setup_len = 4;              /* what 0 has always meant */
    setup_len = (setup_len + 1u) * 512u;
    if (setup_len > sizeof g_setup) {
        say("the kernel's setup is longer than the protocol allows");
        return -1;
    }
    if (kernel->read(kernel->ctx, 0, setup_len, g_setup) != 0) {
        say("the kernel's setup would not read");
        return -1;
    }
    if (kernel->size <= setup_len) {
        say("the kernel is all setup and no kernel");
        return -1;
    }
    pm_len = kernel->size - setup_len;

    /* Where the protected-mode half may go. A relocatable kernel takes any
     * address aligned to kernel_alignment; one that is not takes its
     * preferred address and nothing else. */
    align = rd32(g_setup, HDR_KERNEL_ALIGNMENT);
    if (align == 0)
        align = 0x200000u;
    if (rd8(g_setup, HDR_RELOCATABLE) == 0) {
        load_at = rd64(g_setup, HDR_PREF_ADDRESS);
        if (load_at == 0) {
            say("the kernel is not relocatable and names no address");
            return -1;
        }
    } else {
        load_at = (KERNEL_ADDR + align - 1u) & ~(align - 1u);
    }

    /* init_size is what the kernel needs to decompress and run in, which is
     * larger than the image; the identity map has to cover all of it. */
    init_size = rd32(g_setup, HDR_INIT_SIZE);
    if (init_size < pm_len)
        init_size = pm_len;
    if (load_at + init_size > IDENTITY_MAP) {
        say("the kernel wants more memory than this loader maps");
        return -1;
    }

    /* The protected-mode half only: the setup that precedes it in the file
     * is the sixteen-bit entry, which this route does not take. */
    if (load_object(kernel, setup_len, pm_len, load_at) != 0) {
        say("the kernel would not read");
        return -1;
    }

    /* The command line. Its cap is the kernel's own, minus the terminator. */
    cmdline_cap = rd32(g_setup, HDR_CMDLINE_SIZE);
    if (cmdline_cap == 0 || cmdline_cap > 4096u)
        cmdline_cap = 4096u;
    cmdlen = 0;
    if (cmdline != (const char *)0) {
        while (cmdline[cmdlen] != '\0' && cmdlen + 1u < cmdline_cap)
            cmdlen++;
        memcpy(cmd, cmdline, cmdlen);
    }
    cmd[cmdlen] = '\0';

    /* The initrds, concatenated, as high as the kernel will take them: the
     * low end of memory is where everything else already is. */
    for (i = 0; i < n_initrd; i++)
        initrd_len += initrd[i].size;
    if (initrd_len > 0) {
        initrd_max = rd32(g_setup, HDR_INITRD_ADDR_MAX);
        if (initrd_max == 0)
            initrd_max = 0x37FFFFFFu;
        if (initrd_max >= IDENTITY_MAP)
            initrd_max = IDENTITY_MAP - 1u;

        initrd_at = e820_place_high(initrd_len, 4096u,
                                    load_at + init_size,
                                    (uint64_t)initrd_max + 1u);
        if (initrd_at == 0) {
            say("there is nowhere to put the initrd");
            return -1;
        }
        {
            uint64_t at = initrd_at;

            for (i = 0; i < n_initrd; i++) {
                if (load_object(&initrd[i], 0, initrd[i].size, at) != 0) {
                    say("an initrd would not read");
                    return -1;
                }
                at += initrd[i].size;
            }
        }
    }

    /* ---- the zero page ----
     *
     * Cleared, then the setup header copied in at the offset it occupies in
     * the boot sector, then the fields this loader owns written back over
     * it. The order matters: every one of those fields is inside the range
     * the copy just overwrote. */
    memset(zp, 0, 4096u);
    {
        size_t hdr_end = (size_t)0x202u + rd8(g_setup, HDR_JUMP_LEN);

        if (hdr_end > 0x280u)
            hdr_end = 0x280u;       /* the zero page's header room */
        memcpy(zp + HDR_SETUP_SECTS, g_setup + HDR_SETUP_SECTS,
               hdr_end - HDR_SETUP_SECTS);
    }

    wr8(zp, HDR_TYPE_OF_LOADER, LOADER_TYPE_UNKNOWN);
    wr32(zp, HDR_CODE32_START, (uint32_t)load_at);
    wr8(zp, HDR_LOADFLAGS,
        (uint8_t)((rd8(zp, HDR_LOADFLAGS) | LOADFLAG_LOADED_HIGH) &
                  (uint8_t)~LOADFLAG_CAN_USE_HEAP));
    wr16(zp, HDR_HEAP_END_PTR, 0);
    wr32(zp, HDR_CMD_LINE_PTR, (uint32_t)CMDLINE_ADDR);
    wr32(zp, HDR_RAMDISK_IMAGE, (uint32_t)initrd_at);
    wr32(zp, HDR_RAMDISK_SIZE, (uint32_t)initrd_len);
    fill_e820(zp);

    bios_puts("falcon-bios: kernel at ");
    bios_put_hex64(load_at);
    bios_puts(", ");
    bios_put_dec(pm_len);
    bios_puts(" bytes");
    if (initrd_len > 0) {
        bios_puts("; initrd at ");
        bios_put_hex64(initrd_at);
        bios_puts(", ");
        bios_put_dec(initrd_len);
        bios_puts(" bytes");
    }
    bios_puts("\nfalcon-bios: cmdline: ");
    bios_puts(cmd);
    bios_puts("\nfalcon-bios: entering the kernel\n");

    enter_kernel(load_at + ENTRY64_OFFSET, ZEROPAGE_ADDR);
    return -1;
}
