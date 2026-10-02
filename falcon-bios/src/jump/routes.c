/* falcon-bios: jump's routes through its drivers. See routes.h.
 *
 * Lifted out of apps/jump/bios_main.c (DESIGN Section 12.10) so that jump
 * and station-legacy boot a CD, a stick and a card one way.
 */

#include <start/bios.h>

#include <jump/routes.h>
#include <jump/jmodload.h>
#include <jump/jres.h>
#include <mbr/mbr.h>
#include <mbr/chain.h>
#include <mem/lowmem.h>
#include <console/screen.h>
#include <mem/e820.h>
#include <iso/vcd.h>
#include <vfd/vfd.h>
#include <disk/bootpart.h>

#include <hw/pci.h>

#include <part/table.h>
#include <media/eltorito.h>
#include <lib/string.h>

/* The BDA's count of fixed disks, which is how many drive numbers from 0x80
 * up the BIOS answers for. */
#define BDA_NDISKS 0x475u

/* The drive number ATA.MOD answers for through the reverse thunk: private,
 * raw 2048-byte sectors, never offered to anything but the virtual CD that
 * serves the disc from it as VCD_DRIVE. */
#define CD_RAW_DRIVE 0xEFu

/* The partition entries read to find a falcon stick. */
#define PARTS_MAX 16u

/* The PCI classes, class << 16 | subclass << 8 | programming interface. */
#define PCI_UHCI 0x0C0300u
#define PCI_OHCI 0x0C0310u
#define PCI_EHCI 0x0C0320u

static fs_vol *g_vol;
static int     g_have_ata;
static int     g_have_pcc;

/* What the PCI scan found (DESIGN Section 13.2): the routes a controller
 * exists for. */
static int g_pci_ide;
static int g_pci_cardbus;

/* The USB modules, in the order the route tries them: each needs its file
 * on the volume and its controller on the bus. */
typedef struct {
    const char *name;
    const char *path;
    const char *loading;        /* the route's stage while it is read */
    unsigned    slot;
    uint32_t    pci;
    int         have;           /* file and controller both there */
} usb_mod;

static usb_mod g_usb[] = {
    /* EHCI first: it hands a device that is not high speed to its
     * companion, where the UHCI or OHCI module after it finds it. */
    { "EHCI", "/EHCI.MOD", "loading EHCI.MOD", JMOD_SLOT_EHCI, PCI_EHCI, 0 },
    { "UHCI", "/UHCI.MOD", "loading UHCI.MOD", JMOD_SLOT_UHCI, PCI_UHCI, 0 },
    { "OHCI", "/OHCI.MOD", "loading OHCI.MOD", JMOD_SLOT_OHCI, PCI_OHCI, 0 },
};
#define N_USB (sizeof g_usb / sizeof g_usb[0])

/* The keyboards up (DESIGN Section 13.6): the int16 entries of the USB
 * modules that found one, from the last USB route that brought them up,
 * and the slot of the first, whose stack the keyboard block runs them on.
 * A route that comes back keeps them, since taking the controllers ended
 * the BIOS's own USB keyboard for good; stage 3 polls them itself
 * (bios_key_poll), and the next handover that leaves the keyboard to the
 * BIOS installs them behind INT 16h. */
static uint32_t g_kbd[N_USB];
static unsigned g_nkbd;
static unsigned g_kbd_slot;

/* The module region, carved out of the map a handover passes on while a
 * module stays: every slot, whichever modules are in it. */
static const e820_range k_keep_mods = {
    JMOD_REGION, JMOD_REGION_END - JMOD_REGION
};

static int has_file(const char *path)
{
    fs_dir scratch;
    fs_dirent e;

    return g_vol != (fs_vol *)0 &&
           fs_lookup(g_vol, path, &scratch, &e) == FS_OK && !e.is_dir;
}

void jroute_init(fs_vol *vol, const char *medium)
{
    pci_loc loc;
    unsigned i;

    g_vol = vol;
    jmod_set_medium(medium);
    g_have_ata = has_file("/ATA.MOD");
    g_have_pcc = has_file("/PCC.MOD");
    for (i = 0; i < N_USB; i++)
        g_usb[i].have = has_file(g_usb[i].path);

    /* Any IDE controller, whatever its programming interface says about
     * its modes: one pass, the interface masked off. */
    g_pci_ide = pci_find_class_mask(0x010100u, 0xFFFF00u, 0, &loc) == 0;
    g_pci_cardbus = pci_find_class(0x060700u, 0, &loc) == 0;
    bios_puts("falcon-bios: PCI: ");
    bios_puts(g_pci_ide ? "IDE " : "");
    bios_puts(g_pci_cardbus ? "CardBus " : "");
    for (i = 0; i < N_USB; i++) {
        int found = pci_find_class(g_usb[i].pci, 0, &loc) == 0;

        bios_puts(found ? g_usb[i].name : "");
        bios_puts(found ? " " : "");
        g_usb[i].have = g_usb[i].have && found;
    }
    bios_puts("\n");
}

int jroute_offered(jroute k)
{
    unsigned i;

    switch (k) {
    case JROUTE_CD:
        return g_have_ata && g_pci_ide;
    case JROUTE_CF:
        return g_have_pcc && g_pci_cardbus;
    case JROUTE_USB:
        for (i = 0; i < N_USB; i++) {
            if (g_usb[i].have)
                return 1;
        }
        return 0;
    case JROUTE_COUNT:
    default:
        return 0;
    }
}

const char *jroute_code(jroute k)
{
    switch (k) {
    case JROUTE_CD:  return "CDR";
    case JROUTE_USB: return "USB";
    case JROUTE_CF:  return "PCC";
    case JROUTE_COUNT:
    default:         return "";
    }
}

/* A device read straight through its module, in protected mode, before
 * anything is resident: a disc's catalog, a floppy-emulation image, a
 * stick's partition table. */
static struct {
    jmod_ops *ops;
    unsigned  idx;
} g_disc;

static uint32_t g_disc_sector = 2048u;

static int disc_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    uint32_t ss = g_disc_sector;

    (void)ctx;
    if (off % ss != 0 || len % ss != 0)
        return -1;
    return g_disc.ops->read(g_disc.idx, off / ss, (uint32_t)(len / ss),
                            buf) == 0 ? 0 : -1;
}

/* A stage that reads a module off the floppy or brings up a device, each
 * of which can take seconds: said on the route's page as it starts, so the
 * screen shows where the time goes, and kept as the stage a failure names. */
static void step(boot_result *r, const char *what)
{
    r->stage = what;
    screen_out(what);
    screen_out("...\n");
}

/* CDR: ATA.MOD, the first drive with a disc, and the disc booted as its
 * catalog says. A no-emulation disc is served by the stick's virtual CD
 * from the module's private drive, with the module left resident behind
 * the reverse thunk; a floppy-emulation disc's image is read into memory
 * through the module and served by the stick's virtual floppy, and nothing
 * of the module stays. */
static void boot_cd(boot_result *r)
{
    static jmod_ops ops;
    static elt_catalog cat;
    const elt_entry *e;
    bios_disk raw;
    jmod_dev dev;
    const char *why = (const char *)0;
    uint32_t base = 0;
    unsigned n;
    unsigned i;

    step(r, "loading ATA.MOD");
    if (jmod_load(g_vol, "/ATA.MOD", JMOD_SLOT_ATA, &ops, &why) != 0) {
        r->advice = why;
        return;
    }
    step(r, "looking for a CD drive");
    n = ops.probe();
    for (i = 0; i < n; i++) {
        if (ops.dev(i, &dev) == 0 && dev.sectors != 0)
            break;
    }
    if (i == n) {
        r->advice = n == 0 ? "there is no CD drive on the IDE channels."
                           : "there is no disc in the CD drive.";
        return;
    }
    screen_out(dev.label);
    screen_out("\n\n");

    r->stage = "reading the disc's boot catalog";
    g_disc.ops = &ops;
    g_disc.idx = i;
    g_disc_sector = 2048u;
    if (elt_parse(&cat, disc_read, &g_disc, dev.sectors * 2048u) != ELT_OK) {
        r->advice = "the disc has no El Torito boot catalog.";
        return;
    }
    e = vcd_boot_entry(&cat);
    if (e == (const elt_entry *)0) {
        r->advice = "the disc boots only on UEFI.";
        return;
    }
    if (e->media == ELT_MEDIA_1200K || e->media == ELT_MEDIA_1440K ||
        e->media == ELT_MEDIA_2880K) {
        /* Never returns on success. A DOS boot disc reads its keyboard
         * through the BIOS, so USB keyboards that are up go behind INT 16h
         * first. */
        int kb = jroute_kbd_install();

        (void)vfd_serve(disc_read, &g_disc, e, r);
        if (kb == 0)
            jroute_kbd_remove();
        return;
    }
    if (e->media != ELT_MEDIA_NONE) {
        r->advice = "the disc's boot image asks for hard-disk emulation, "
                    "which this loader does not do.";
        return;
    }

    r->stage = "installing the CD driver";
    jres_keyboards(g_kbd, g_nkbd);
    if (ops.serve(i, (uint8_t)CD_RAW_DRIVE) != 0 ||
        jres_install((uint8_t)CD_RAW_DRIVE, (uint32_t)(uintptr_t)ops.int13,
                     JMOD_SLOT_ADDR(JMOD_SLOT_ATA) + JMOD_SLOT_BYTES,
                     (const e820_range *)0, 0, &base) != 0) {
        r->advice = "this machine will not give up the conventional memory "
                    "the CD driver needs.";
        return;
    }
    /* Through the thunk, as the disc's own bootloader will reach it. */
    if (!bios_disk_probe(&raw, (uint8_t)CD_RAW_DRIVE)) {
        r->advice = "the CD driver did not answer through INT 13h.";
        jres_remove();
        return;
    }
    vcd_keep_reset();
    (void)vcd_keep(base, JRES_BYTES);
    (void)vcd_keep(JMOD_REGION, JMOD_REGION_END - JMOD_REGION);
    (void)vcd_chainload_drive(&raw, dev.sectors, r);
    jres_remove();
}

/* Whether the device the module calls `idx` is a falcon disk: a partition
 * on it, read through the module, carries a `boot` record, which is what
 * the disk's own stage 2 will look for once it is started (DESIGN Section
 * 3.3). Any install's: the app has none of its own to match. */
static int is_falcon_disk(jmod_ops *ops, unsigned idx)
{
    static part_entry parts[PARTS_MAX];
    static uint8_t sec[512];
    bootpart bp;

    g_disc.ops = ops;
    g_disc.idx = idx;
    g_disc_sector = 512u;
    return bootpart_find(disc_read, &g_disc, 512u, 0, parts, PARTS_MAX, sec,
                         &bp) == BOOTPART_OK;
}

/* A disk a module serves, booted as the BIOS disk after the machine's own:
 * the module resident behind the reverse thunk with its memory carved out
 * of INT 15h, the BDA's disk count one higher while it runs, and the disk's
 * LBA 0 at 0x7C00. A falcon disk is handed over with DL naming it, any
 * other behind the station's drive swap (chain.c) with DL 0x80 (DESIGN
 * Section 13.6 has the rule). Returns only when it could not hand over,
 * with everything it installed taken back. */
static void boot_module_disk(jmod_ops *ops, unsigned idx, unsigned slot,
                             boot_result *r)
{
    bios_disk disk;
    uint32_t base = 0;
    uint8_t ndisks = peek8(BDA_NDISKS);
    uint8_t drive = (uint8_t)(0x80u + ndisks);
    int swap = !is_falcon_disk(ops, idx);

    r->stage = "installing the driver";
    jres_keyboards(g_kbd, g_nkbd);
    if (ndisks >= 0x7Fu || ops->serve(idx, drive) != 0 ||
        jres_install(drive, (uint32_t)(uintptr_t)ops->int13,
                     JMOD_SLOT_ADDR(slot) + JMOD_SLOT_BYTES, &k_keep_mods, 1u,
                     &base) != 0) {
        r->advice = "this machine will not give up the conventional memory "
                    "the driver needs.";
        return;
    }
    poke8(BDA_NDISKS, (uint8_t)(ndisks + 1u));
    if (!bios_disk_probe(&disk, drive)) {
        r->advice = "the driver did not answer through INT 13h.";
    } else if (!swap) {
        bios_puts("falcon-bios: drive ");
        bios_put_hex64(drive);
        bios_puts(" carries a falcon boot record; no drive swap\n");
        r->stage = "starting the disk";
        (void)mbr_boot(&disk, r);
    } else if (lowmem_segment(SWAP_BYTES) == 0) {
        r->stage = "installing the drive swap";
        r->advice = "this machine will not give up the conventional memory "
                    "the drive swap needs.";
    } else {
        bios_puts("falcon-bios: drive ");
        bios_put_hex64(drive);
        bios_puts(" carries no falcon boot record; swapping it with 0x80\n");
        (void)chain_disk(&disk, 1, r);
    }
    poke8(BDA_NDISKS, ndisks);
    jres_remove();
}

/* PCC: PCC.MOD, the first CompactFlash card in a PC Card slot, booted as a
 * disk: its own MBR, whatever it holds, as the internal disks are. */
static void boot_cf(boot_result *r)
{
    static jmod_ops ops;
    jmod_dev dev;
    const char *why = (const char *)0;
    unsigned n;

    step(r, "loading PCC.MOD");
    if (jmod_load(g_vol, "/PCC.MOD", JMOD_SLOT_PCC, &ops, &why) != 0) {
        r->advice = why;
        return;
    }
    step(r, "bringing up the PC Card slots");
    n = ops.probe();
    if (n == 0 || ops.dev(0, &dev) != 0) {
        r->advice = "there is no CompactFlash card in a PC Card slot.";
    } else {
        screen_out(dev.label);
        screen_out("\n\n");
        boot_module_disk(&ops, 0, JMOD_SLOT_PCC, r);
    }
    /* Back without a handover: the slots powered down, their windows
     * closed. */
    if (ops.release != (void (*)(void))0)
        ops.release();
}

/* USB: the first falcon stick on any controller a module is there for
 * (EHCI, then UHCI, then OHCI), booted as the next BIOS disk
 * (boot_module_disk); the stick's own stage 1 takes it from there. Every
 * module is brought up, the ones after the stick's too: taking a controller
 * ends the BIOS's keyboard emulation on it and on EHCI's companions, and a
 * module answers for the keyboards it found, through the resident's INT 16h
 * hook after a handover and through stage 3's own poll when the route
 * comes back (DESIGN Section 13.6). */
static void boot_usb(boot_result *r)
{
    static jmod_ops ops[N_USB];
    jmod_dev dev;
    const char *why = (const char *)0;
    unsigned found_any = 0;
    unsigned stick = N_USB;
    unsigned at = 0;
    int refused = 0;
    unsigned m;

    /* The modules are read over whatever an earlier run left up. */
    g_nkbd = 0;
    for (m = 0; m < N_USB; m++) {
        unsigned n;
        unsigned i;

        if (!g_usb[m].have)
            continue;
        step(r, g_usb[m].loading);
        if (jmod_load(g_vol, g_usb[m].path, g_usb[m].slot, &ops[m],
                      &why) != 0) {
            if (stick == N_USB) {
                r->advice = why;
                refused = 1;
                break;
            }
            continue;
        }
        step(r, stick == N_USB ? "looking for a falcon stick"
                               : "looking for a keyboard");
        n = ops[m].probe();
        found_any += n;
        if (ops[m].int16 != 0) {
            if (g_nkbd == 0)
                g_kbd_slot = g_usb[m].slot;
            g_kbd[g_nkbd++] = (uint32_t)(uintptr_t)ops[m].int16;
        }
        for (i = 0; stick == N_USB && i < n; i++) {
            if (ops[m].dev(i, &dev) == 0 && is_falcon_disk(&ops[m], i)) {
                stick = m;
                at = i;
            }
        }
    }
    if (refused) {
        /* The advice names the module that was refused. */
    } else if (stick == N_USB) {
        r->advice = found_any == 0 ? "there is no USB storage on the ports."
                                   : "none of the USB storage is a falcon stick.";
    } else {
        (void)ops[stick].dev(at, &dev);
        screen_out(dev.label);
        screen_out("\n\n");
        boot_module_disk(&ops[stick], at, g_usb[stick].slot, r);
    }
    if (g_nkbd != 0) {
        bios_puts("falcon-bios: USB: the route came back; ");
        bios_put_dec(g_nkbd);
        bios_puts(g_nkbd == 1u ? " module's keyboards stay up\n"
                               : " modules' keyboards stay up\n");
    }
}

int bios_key_poll(void)
{
    jmod_regs regs;
    unsigned i;

    if (g_nkbd == 0)
        return 0;
    /* Stage 3 runs in protected mode with interrupts off, as the reverse
     * thunk calls these, so the entry is called here directly; it ignores
     * the registers, as it does the hook's. */
    for (i = 0; i < g_nkbd; i++) {
        memset(&regs, 0, sizeof regs);
        ((void (*)(jmod_regs *))(uintptr_t)g_kbd[i])(&regs);
    }
    return 1;
}

int jroute_kbd_install(void)
{
    if (g_nkbd == 0)
        return 1;
    jres_keyboards(g_kbd, g_nkbd);
    if (jres_install_keyboards(JMOD_SLOT_ADDR(g_kbd_slot) + JMOD_SLOT_BYTES,
                               &k_keep_mods, 1u) != 0) {
        jres_keyboards((const uint32_t *)0, 0);
        bios_puts("falcon-bios: no conventional memory for the keyboard "
                  "block; the USB keyboards end at the handover\n");
        return -1;
    }
    return 0;
}

void jroute_kbd_remove(void)
{
    jres_remove();
}

void jroute_kbd_release(void)
{
    if (g_nkbd == 0)
        return;
    g_nkbd = 0;
    bios_puts("falcon-bios: USB: the keyboards are released; only a PS/2 "
              "keyboard answers from here\n");
}

void jroute_boot(jroute k, boot_result *r)
{
    switch (k) {
    case JROUTE_CD:
        boot_cd(r);
        return;
    case JROUTE_USB:
        boot_usb(r);
        return;
    case JROUTE_CF:
        boot_cf(r);
        return;
    case JROUTE_COUNT:
    default:
        r->stage = "choosing a route";
        return;
    }
}
