/* falcon-bios: booting a Linux kernel directly.
 *
 * The 32-bit entry of the x86 boot protocol, which is the one route into a
 * kernel that does not require going back to real mode, and the one every
 * kernel has: a 64-bit kernel enters at code32_start exactly as a 32-bit one
 * does and switches to long mode itself. The loader is already in flat
 * 32-bit protected mode with paging off, which is exactly the state the
 * protocol asks for, so the handoff is: read the setup header out of the
 * image, load the protected-mode half somewhere it will accept, fill in a
 * zero page, and jump.
 *
 * WHAT THE PROTOCOL REQUIRES AT THE JUMP, and every one of these is a way to
 * fail silently if it is wrong:
 *
 *   - 32-bit protected mode, paging off, with the kernel, the initrd, the
 *     zero page and the command line all below 4 GiB (biosdefs.h,
 *     ADDR_END), which bounds where any of them may go.
 *   - interrupts off, and no interrupt handler expected to survive.
 *   - a GDT whose code selector is 0x10 and data selector 0x18, both flat
 *     4 GiB, and CS, DS, ES and SS loaded with them. Ours is not laid out
 *     that way: falcon-bios puts its code at 0x08. So a second, handoff GDT
 *     is built for the jump.
 *   - %esi holding the zero page, and %ebp, %edi and %ebx zero.
 *
 * WHAT THE ZERO PAGE IS. The first 0x1F1 bytes are fields the boot loader
 * fills in and the kernel reads; from 0x1F1 to the end of the setup header
 * is a copy of the header out of the kernel image itself, with a handful of
 * fields written back. It is not a structure this file declares, because
 * declaring it would mean declaring every field of a layout that has grown
 * for thirty years; the ones that matter are named offsets, which is how the
 * kernel's own documentation describes them.
 */

#include <start/bios.h>
#include <linux/linux.h>
#include <linux/linuxcore.h>
#include <mem/e820.h>
#include <media/bootcfg.h>

#include <lib/string.h>

#include <cpuid.h>

/* Setup header offsets, from the kernel's Documentation/arch/x86/boot.rst.
 * Offsets into the boot sector, which is also where they sit in the zero
 * page: the header is copied across at the same address. */

#define LOADFLAG_LOADED_HIGH 0x01u
#define LOADFLAG_CAN_USE_HEAP 0x80u
#define LOADER_TYPE_UNKNOWN 0xFFu         /* no id assigned to falcon-boot */

/* One boot sector plus the setup sectors, read before anything else so the
 * header can be consulted. The protocol caps setup at 64 sectors. */
/* The zero page's own fields, which are not the setup header's: the header
 * is copied into the page and these sit outside the range it occupies. */
#define BP_E820_ENTRIES     0x1E8   /* u8 */
#define BP_E820_TABLE       0x2D0   /* E820_MAX * 20 bytes */
/* struct screen_info, at the head of the zero page. */
#define BP_ORIG_X           0x00    /* u8, cursor column */
#define BP_ORIG_Y           0x01    /* u8, cursor row */
#define BP_VIDEO_PAGE       0x04    /* u16 */
#define BP_VIDEO_MODE       0x06    /* u8 */
#define BP_VIDEO_COLS       0x07    /* u8 */
#define BP_VIDEO_EGA_BX     0x0A    /* u16 */
#define BP_VIDEO_LINES      0x0E    /* u8 */
#define BP_VIDEO_ISVGA      0x0F    /* u8 */
#define BP_VIDEO_POINTS     0x10    /* u16, character height */

/* The BIOS data area's copy of the text screen's shape. */
#define BDA_COLS            0x44Au  /* u16 */
#define BDA_ROWS_MINUS_1    0x484u  /* u8 */
#define BDA_POINTS          0x485u  /* u16 */

static uint8_t g_setup[65u * 512u];

/* The handoff GDT. Selector 0x10 is code, 0x18 is data, because that is what
 * the kernel's entry assumes; the null and unused entries are there to put
 * them at those indices and nothing else. */
static uint64_t g_gdt[4] __attribute__((aligned(16))) = {
    0,
    0,
    0x00CF9A000000FFFFull,   /* 0x10: code32, flat 4 GiB */
    0x00CF92000000FFFFull    /* 0x18: data32, flat 4 GiB */
};

static struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
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

int linux_probe(const linux_object *kernel, int *is_64)
{
    linux_geom geom;

    if (kernel == (const linux_object *)0 || is_64 == (int *)0 ||
        kernel->read(kernel->ctx, 0, 1024, g_setup) != 0 ||
        linuxcore_geometry(g_setup, 1024u, kernel->size, KERNEL_ADDR,
                           ADDR_END, sizeof g_setup, &geom) != LINUXCORE_OK)
        return -1;
    *is_64 = geom.is_64;
    return 0;
}

int linux_cpu_is_64(void)
{
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;

    /* stage2.S has required CPUID, so the leaf-0x80000000 probe cannot
     * fault; a processor without extended leaves answers below 0x80000001. */
    if (__get_cpuid(0x80000000u, &a, &b, &c, &d) == 0 || a < 0x80000001u)
        return 0;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d) == 0)
        return 0;
    return (d & (1u << 29)) != 0;   /* LM */
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
    case LINUXCORE_ERR_SETUP_LONG:
        return "the kernel's setup is longer than the protocol allows";
    case LINUXCORE_ERR_NO_KERNEL:   return "the kernel is all setup and no kernel";
    case LINUXCORE_ERR_NOT_RELOC:
        return "the kernel is not relocatable and names no address";
    case LINUXCORE_ERR_NO_ROOM:
        return "the kernel wants more memory than this loader can address";
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

/* Whether the last linux_boot refused before it placed anything, because
 * what it was handed is not a kernel it can start (see linux_not_taken). */
static int g_not_taken;

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

int linux_not_taken(void)
{
    return g_not_taken;
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

static void int10(uint32_t eax, uint32_t ebx)
{
    thunk_regs.eax = eax;
    thunk_regs.ebx = ebx;
    thunk_regs.ecx = 0;
    thunk_regs.edx = 0;
    thunk_vector = 0x10;
    thunk_bios();
}

/* The text screen, described in the zero page the way the kernel's own
 * real-mode setup describes it (arch/x86/boot/video.c and video-vga.c),
 * because the 32-bit entry skips that code and the zero page is otherwise
 * zero. A zero screen_info has no rows and no columns, which vgacon reads
 * as "no VGA": the kernel then runs on the dummy console, and on a kernel
 * whose initramfs brings no GPU driver nothing it or systemd prints ever
 * reaches the screen. The machine boots and shows a black screen.
 *
 * Asked of the BIOS, not assumed, and asked last, after the route's own
 * final lines, so the cursor it reports is where the kernel's first line
 * belongs. screen.c has set 80x25 colour text by the time any route runs. */
static void fill_screen_info(uint8_t *zp)
{
    int10(0x0300u, 0);                  /* AH=03h, cursor of page 0 */
    wr8(zp, BP_ORIG_X, (uint8_t)(thunk_regs.edx & 0xFFu));
    wr8(zp, BP_ORIG_Y, (uint8_t)((thunk_regs.edx >> 8) & 0xFFu));

    int10(0x0F00u, 0);                  /* AH=0Fh, current mode and page */
    wr8(zp, BP_VIDEO_MODE, (uint8_t)(thunk_regs.eax & 0x7Fu));
    wr16(zp, BP_VIDEO_PAGE, (uint16_t)((thunk_regs.ebx >> 8) & 0xFFu));

    wr8(zp, BP_VIDEO_COLS, (uint8_t)peek16(BDA_COLS));
    wr8(zp, BP_VIDEO_LINES, (uint8_t)(peek8(BDA_ROWS_MINUS_1) + 1u));
    wr16(zp, BP_VIDEO_POINTS, peek16(BDA_POINTS));

    /* AH=12h BL=10h is the EGA information call: BL coming back unchanged
     * means no EGA, and vgacon reads the same field to decide. AX=1A00h
     * answers 1Ah in AL only on VGA. */
    int10(0x1200u, 0x0010u);
    wr16(zp, BP_VIDEO_EGA_BX, (uint16_t)(thunk_regs.ebx & 0xFFFFu));
    if ((thunk_regs.ebx & 0xFFu) != 0x10u) {
        int10(0x1A00u, 0);
        if ((thunk_regs.eax & 0xFFu) == 0x1Au)
            wr8(zp, BP_VIDEO_ISVGA, 1u);
    }
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

/* The move for a staged kernel (linuxcore.h, stage_at): copy it down over
 * the loader, then enter it. Copied to LINUX_STUB_ADDR before it runs, so it
 * is position independent and uses nothing but registers: ESI the staged
 * copy, EDI where it runs, ECX its length, EDX the entry, EBX the zero page.
 * Nothing of the loader survives the copy, this included in its original
 * place, which is why it runs from the copy. */
__asm__(
    "\t.section .text.linux_move, \"ax\"\n"
    "\t.code32\n"
    "\t.globl linux_move_stub\n"
    "linux_move_stub:\n"
    "\tcld\n"
    "\trep movsb\n"
    "\tmovl   %ebx, %esi\n"
    "\txorl   %ebp, %ebp\n"
    "\txorl   %edi, %edi\n"
    "\txorl   %ebx, %ebx\n"
    "\tjmp    *%edx\n"
    "linux_move_stub_end:\n"
    "\t.previous\n"
    /* Its length, computed by the assembler: C subtracting two symbols is
     * pointer arithmetic across objects, which C does not define. */
    "\t.section .rodata.linux_move, \"a\"\n"
    "\t.globl linux_move_stub_len\n"
    "\t.balign 4\n"
    "linux_move_stub_len:\n"
    "\t.long linux_move_stub_end - linux_move_stub\n"
    "\t.previous\n");

extern const uint8_t linux_move_stub[];
extern const uint32_t linux_move_stub_len;

/* Never returns. Everything the kernel needs is in memory; this is the
 * handoff itself, and it is the fifth and last of the assembly jobs
 * bios.h lists, written here as inline asm because it is six instructions and
 * separating them from the values they use would help nobody.
 *
 * A nonzero `move` is a staged kernel: `move` bytes at `from` go to `entry`
 * first, through the stub above. The GDT goes below a megabyte either way,
 * since the move would otherwise copy the kernel over the table the CPU is
 * still reading its segments from. */
static void enter_kernel(uint32_t entry, uint32_t zeropage, uint32_t from,
                         uint32_t move)
{
    memcpy((void *)(uintptr_t)LINUX_GDT_ADDR, g_gdt, sizeof g_gdt);
    g_gdtr.limit = (uint16_t)(sizeof g_gdt - 1u);
    g_gdtr.base = (uint32_t)LINUX_GDT_ADDR;

    if (move != 0) {
        memcpy((void *)(uintptr_t)LINUX_STUB_ADDR, linux_move_stub,
               (size_t)linux_move_stub_len);
        __asm__ __volatile__(
            "cli\n\t"
            "lgdt   %[gdtr]\n\t"
            "ljmp   $0x10, $1f\n"
            "1:\n\t"
            "movw   $0x18, %%ax\n\t"
            "movw   %%ax, %%ds\n\t"
            "movw   %%ax, %%es\n\t"
            "movw   %%ax, %%ss\n\t"
            "movw   %%ax, %%fs\n\t"
            "movw   %%ax, %%gs\n\t"
            "ljmp   $0x10, %[stub]\n\t"
            :
            : [gdtr] "m"(g_gdtr), [stub] "i"(LINUX_STUB_ADDR),
              "S"(from), "D"(entry), "c"(move), "d"(entry), "b"(zeropage)
            : "memory");
        for (;;)
            __asm__ __volatile__("hlt");
    }

    /* Nothing is declared clobbered because nothing comes back: after the
     * last jump this loader no longer exists, and the registers the
     * protocol names are set by hand. */
    __asm__ __volatile__(
        "cli\n\t"
        "lgdt   %[gdtr]\n\t"
        /* CS first, through a far jump to the next instruction, then the
         * data selectors: the protocol wants all four loaded from the
         * handoff GDT, not merely present in it. */
        "ljmp   $0x10, $1f\n"
        "1:\n\t"
        "movw   $0x18, %%ax\n\t"
        "movw   %%ax, %%ds\n\t"
        "movw   %%ax, %%es\n\t"
        "movw   %%ax, %%ss\n\t"
        "movw   %%ax, %%fs\n\t"
        "movw   %%ax, %%gs\n\t"
        "xorl   %%ebp, %%ebp\n\t"
        "xorl   %%edi, %%edi\n\t"
        "xorl   %%ebx, %%ebx\n\t"
        "jmp    *%[entry]\n\t"
        :
        : [gdtr] "m"(g_gdtr), [entry] "d"(entry), "S"(zeropage)
        : "memory");
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
    uint64_t initrd_at = 0;
    uint64_t initrd_len = 0;
    uint32_t cmdline_cap;
    size_t cmdlen;
    unsigned i;
    linux_geom geom;
    linuxcore_status rc;

    g_not_taken = 1;
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
                            ADDR_END, sizeof g_setup, &geom);
    if (rc != LINUXCORE_OK) {
        say(geom_error(rc));
        return -1;
    }
    /* From here a refusal is not a cue to try the medium's own loader: a
     * 64-bit kernel on a processor without long mode is the same kernel
     * that loader would start. */
    g_not_taken = 0;
    /* Asked here rather than found out: a 64-bit kernel on a processor
     * without long mode dies in its own decompressor, before it can print
     * anything, and the screen keeps saying "starting". */
    if (geom.is_64 && !linux_cpu_is_64()) {
        say("the kernel is 64-bit and this processor has no 64-bit mode");
        return -1;
    }
    /* Where it runs, and where it is staged, have to be memory: a machine
     * with less than the kernel's footprint, or something kept resident in
     * the way (falcon jump's drivers at 15 MB, carved out of the map this
     * loader was handed), would otherwise take the kernel's first write. */
    if (!e820_is_ram(geom.load_at, geom.init_size) ||
        (geom.stage_at != 0 && !e820_is_ram(geom.stage_at, geom.pm_len))) {
        say("the kernel needs memory this machine's map does not have "
            "free where it has to go");
        return -1;
    }
    setup_len = geom.setup_len;
    pm_len = geom.pm_len;
    load_at = geom.load_at;

    /* Now the whole setup, which the zero page is built out of. */
    if (kernel->read(kernel->ctx, 0, setup_len, g_setup) != 0) {
        say("the kernel's setup would not read");
        return -1;
    }

    /* The protected-mode half only: the setup that precedes it in the file
     * is the sixteen-bit entry, which this route does not take. A staged
     * kernel is read to its staging address and moved at the handoff. */
    if (load_object(kernel, setup_len, pm_len,
                    geom.stage_at != 0 ? geom.stage_at : load_at) != 0) {
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
        uint64_t sizes[BOOTCFG_INITRD_MAX + 1u] = { 0 };

        for (i = 0; i < n_initrd && i <= BOOTCFG_INITRD_MAX; i++)
            sizes[i] = initrd[i].size;
        initrd_len = linuxcore_initrd_len(sizes, i);
    }
    if (initrd_len > 0) {
        initrd_at = e820_place_high(initrd_len, 4096u, geom.end,
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
    if (geom.stage_at != 0) {
        bios_puts(", staged at ");
        bios_put_hex64(geom.stage_at);
    }
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
    fill_screen_info(zp);
    enter_kernel((uint32_t)load_at, ZEROPAGE_ADDR, (uint32_t)geom.stage_at,
                 geom.stage_at != 0 ? (uint32_t)pm_len : 0u);
    return -1;
}
