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
#include <linuxcore.h>
#include <e820.h>
#include <media/bootcfg.h>

#include <lib/string.h>

/* Setup header offsets, from the kernel's Documentation/arch/x86/boot.rst.
 * Offsets into the boot sector, which is also where they sit in the zero
 * page: the header is copied across at the same address. */

#define LOADFLAG_LOADED_HIGH 0x01u
#define LOADFLAG_CAN_USE_HEAP 0x80u
#define LOADER_TYPE_UNKNOWN 0xFFu         /* no id assigned to falcon-boot */

/* The entry point sits this far into the loaded protected-mode kernel. */
#define ENTRY64_OFFSET      0x200

/* One boot sector plus the setup sectors, read before anything else so the
 * header can be consulted. The protocol caps setup at 64 sectors. */
/* The zero page's own fields, which are not the setup header's: the header
 * is copied into the page and these sit outside the range it occupies. */
#define BP_E820_ENTRIES     0x1E8   /* u8 */
#define BP_E820_TABLE       0x2D0   /* E820_MAX * 20 bytes */

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

/* What each refusal means, in the loader's voice. Separate from the codes so
 * linuxcore stays free of strings and a test can name a status. */
static const char *geom_error(linuxcore_status rc)
{
    switch (rc) {
    case LINUXCORE_ERR_TOO_SMALL:   return "that is not a kernel";
    case LINUXCORE_ERR_NOT_BZIMAGE:
        return "the kernel has no setup header; it is not a bzImage";
    case LINUXCORE_ERR_VERSION:
        return "the kernel's boot protocol is older than this loader supports";
    case LINUXCORE_ERR_NO_64:       return "the kernel has no 64-bit entry point";
    case LINUXCORE_ERR_SETUP_LONG:
        return "the kernel's setup is longer than the protocol allows";
    case LINUXCORE_ERR_NO_KERNEL:   return "the kernel is all setup and no kernel";
    case LINUXCORE_ERR_NOT_RELOC:
        return "the kernel is not relocatable and names no address";
    case LINUXCORE_ERR_NO_ROOM:
        return "the kernel wants more memory than this loader maps";
    case LINUXCORE_ERR_TOO_LOW:
        return "the kernel is not relocatable and its address is this"
               " loader's own memory";
    case LINUXCORE_ERR_ARG:
    case LINUXCORE_OK:
        break;
    }
    return "the kernel's setup header makes no sense";
}

/* Why the last linux_boot failed, for the caller's screen; serial gets it
 * here as it happens. */
static const char *g_why;

static void say(const char *s)
{
    g_why = s;
    bios_puts("falcon-bios: ");
    bios_puts(s);
    bios_puts("\n");
}

const char *linux_why(void)
{
    return g_why;
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
               unsigned n_initrd, const char *cmdline, void (*last)(void))
{
    uint8_t *zp = (uint8_t *)(uintptr_t)ZEROPAGE_ADDR;
    char *cmd = (char *)(uintptr_t)CMDLINE_ADDR;
    uint64_t setup_len;
    uint64_t pm_len;
    uint64_t load_at;
    uint64_t init_size;
    uint64_t initrd_at = 0;
    uint64_t initrd_len = 0;
    uint32_t cmdline_cap;
    size_t cmdlen;
    unsigned i;
    linux_geom geom;
    linuxcore_status rc;

    if (kernel == (const linux_object *)0) {
        say("that is not a kernel");
        return -1;
    }

    /* Two sectors, because "HdrS" is past the end of the first. */
    if (kernel->read(kernel->ctx, 0, 1024, g_setup) != 0) {
        say("the kernel's first sectors would not read");
        return -1;
    }
    /* Every check and every address is linuxcore's, so the one place this
     * can be got wrong is host-tested. */
    rc = linuxcore_geometry(g_setup, 1024u, kernel->size, KERNEL_ADDR,
                            IDENTITY_MAP, sizeof g_setup, &geom);
    if (rc != LINUXCORE_OK) {
        say(geom_error(rc));
        return -1;
    }
    setup_len = geom.setup_len;
    pm_len = geom.pm_len;
    load_at = geom.load_at;
    init_size = geom.init_size;

    /* Now the whole setup, which the zero page is built out of. */
    if (kernel->read(kernel->ctx, 0, setup_len, g_setup) != 0) {
        say("the kernel's setup would not read");
        return -1;
    }

    /* The protected-mode half only: the setup that precedes it in the file
     * is the sixteen-bit entry, which this route does not take. */
    if (load_object(kernel, setup_len, pm_len, load_at) != 0) {
        say("the kernel would not read");
        return -1;
    }

    /* The command line. Its cap is the kernel's own, minus the terminator. */
    cmdline_cap = geom.cmdline_cap;
    cmdlen = 0;
    if (cmdline != (const char *)0) {
        while (cmdline[cmdlen] != '\0' && cmdlen + 1u < cmdline_cap)
            cmdlen++;
        memcpy(cmd, cmdline, cmdlen);
    }
    cmd[cmdlen] = '\0';

    /* The initrds, concatenated, as high as the kernel will take them: the
     * low end of memory is where everything else already is.
     *
     * EACH ONE IS PADDED TO FOUR BYTES, and it is not a detail. The kernel's
     * unpacker walks a concatenation of archives and expects every segment
     * to begin aligned; a segment that starts one byte late is read as a
     * corrupt header. Debian's initrd is 113,383,147 bytes, so appending
     * falcon-hook straight onto it gave
     *
     *   Initramfs unpacking failed: invalid magic at start of compressed
     *   archive
     *
     * and a kernel with no root. Arch's happens to be a multiple of four,
     * which is why that medium got further and looked like a different bug.
     * falcon-efi/src/direct/direct.c has always padded; this did not. */
    {
        uint64_t sizes[BOOTCFG_INITRD_MAX + 1u];

        for (i = 0; i < n_initrd && i <= BOOTCFG_INITRD_MAX; i++)
            sizes[i] = initrd[i].size;
        initrd_len = linuxcore_initrd_len(sizes, i);
    }
    if (initrd_len > 0) {
        initrd_at = e820_place_high(initrd_len, 4096u,
                                    load_at + init_size,
                                    geom.initrd_max + 1u);
        if (initrd_at == 0) {
            say("there is nowhere to put the initrd");
            return -1;
        }
        {
            uint64_t at = initrd_at;

            for (i = 0; i < n_initrd; i++) {
                uint64_t pad = linuxcore_align4(initrd[i].size) - initrd[i].size;

                if (load_object(&initrd[i], 0, initrd[i].size, at) != 0) {
                    say("an initrd would not read");
                    return -1;
                }
                at += initrd[i].size;
                if (pad != 0) {
                    memset((void *)(uintptr_t)at, 0, (size_t)pad);
                    at += pad;
                }
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

    if (last != (void (*)(void))0)
        last();
    enter_kernel(load_at + ENTRY64_OFFSET, ZEROPAGE_ADDR);
    return -1;
}
