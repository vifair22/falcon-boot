/* Can falcon-bios boot this medium, and does it need a 64-bit processor. See
 * falcon-core/src/media/biosboot.h. */

#include <media/biosboot.h>

#include <media/eltorito.h>
#include <lib/string.h>

/* The x86 boot protocol's setup header (the kernel's
 * Documentation/arch/x86/boot.rst; falcon-bios/src/linux/linuxcore.h names
 * the same offsets for the route that boots it). */
#define SETUP_MAGIC_OFF 0x202u     /* "HdrS" */
#define SETUP_VERSION   0x206u     /* u16, the protocol version */
#define SETUP_XLOAD     0x236u     /* u16, xloadflags */
#define XLF_KERNEL_64   0x0001u
/* 2.12: where xloadflags is present and settled, and the version below
 * which falcon-bios declines a kernel (LINUXCORE_MIN_VERSION). */
#define VERSION_XLOAD   0x020Cu
/* linuxcore_geometry's floor: a kernel file shorter than this is refused
 * before its header is looked at. */
#define KERNEL_MIN      1024u

/* ELF identification and e_machine (the System V gABI). */
#define ELF_CLASS       4u
#define ELF_CLASS32     1u
#define ELF_CLASS64     2u
#define ELF_MACHINE     18u
#define EM_386          3u
#define EM_X86_64       62u

/* Where the BSD route reads the kernel's width from (bsd.c's
 * loader_is_i386): the kernel, because FreeBSD's loader is not a plain ELF
 * file and the kernel always is. */
#define BSD_KERNEL "/boot/kernel/kernel"

static uint16_t rd16(const uint8_t *p, size_t off)
{
    return (uint16_t)((unsigned)p[off] | ((unsigned)p[off + 1u] << 8));
}

biosboot_kernel biosboot_kernel_kind(const void *head, size_t len,
                                     uint64_t file_len)
{
    const uint8_t *b = head;

    if (b == (const uint8_t *)0)
        return BIOSBOOT_KERNEL_UNKNOWN;
    if (file_len < KERNEL_MIN)
        return BIOSBOOT_KERNEL_NONE;
    if (len < SETUP_MAGIC_OFF + 4u)
        return BIOSBOOT_KERNEL_UNKNOWN;
    if (memcmp(b + SETUP_MAGIC_OFF, "HdrS", 4) != 0)
        return BIOSBOOT_KERNEL_NONE;
    if (len < SETUP_XLOAD + 2u || rd16(b, SETUP_VERSION) < VERSION_XLOAD)
        return BIOSBOOT_KERNEL_OLD;
    return (rd16(b, SETUP_XLOAD) & XLF_KERNEL_64) != 0 ? BIOSBOOT_KERNEL_64
                                                       : BIOSBOOT_KERNEL_32;
}

biosboot_answer biosboot_elf_is_64(const void *head, size_t len)
{
    const uint8_t *b = head;
    uint16_t machine;

    if (b == (const uint8_t *)0 || len < ELF_MACHINE + 2u ||
        b[0] != 0x7Fu || b[1] != 'E' || b[2] != 'L' || b[3] != 'F')
        return BIOSBOOT_UNKNOWN;
    machine = rd16(b, ELF_MACHINE);
    if (b[ELF_CLASS] == ELF_CLASS64 && machine == EM_X86_64)
        return BIOSBOOT_YES;
    if (b[ELF_CLASS] == ELF_CLASS32 && machine == EM_386)
        return BIOSBOOT_NO;
    return BIOSBOOT_UNKNOWN;
}

biosboot_answer biosboot_has_image(medium *m)
{
    elt_catalog cat;
    const elt_entry *e;
    int rc;

    if (m == (medium *)0)
        return BIOSBOOT_UNKNOWN;
    rc = elt_parse(&cat, m->read, m->ctx, m->size);
    /* No boot record, or one that is not El Torito's: a data disc, which a
     * BIOS has nothing to start from. A read failure or a catalog that does
     * not check out proves nothing either way. */
    if (rc == ELT_ERR_NOT_ISO || rc == ELT_ERR_NO_CATALOG)
        return BIOSBOOT_NO;
    if (rc != ELT_OK)
        return BIOSBOOT_UNKNOWN;
    e = elt_bios_entry(&cat);
    if (e == (const elt_entry *)0)
        return cat.truncated ? BIOSBOOT_UNKNOWN : BIOSBOOT_NO;
    if (e->media == ELT_MEDIA_NONE || e->media == ELT_MEDIA_1200K ||
        e->media == ELT_MEDIA_1440K || e->media == ELT_MEDIA_2880K)
        return BIOSBOOT_YES;
    return BIOSBOOT_UNKNOWN;
}

/* The volume of the given kind, or NULL. */
static fs_vol *vol_of(medium *m, medium_vol_kind kind)
{
    unsigned i;

    for (i = 0; i < medium_count(m); i++) {
        if (medium_kind_at(m, i) == kind)
            return medium_vol_at(m, i);
    }
    return (fs_vol *)0;
}

/* The BSD route's kernel, by its ELF header, off the ISO9660 volume the
 * route mounts. */
static biosboot_answer bsd_needs64(medium *m, biosboot_work *w)
{
    fs_vol *v = vol_of(m, MEDIUM_VOL_ISO9660);

    if (v == (fs_vol *)0 ||
        fs_lookup(v, BSD_KERNEL, &w->walk, &w->ent) != FS_OK ||
        w->ent.is_dir || w->ent.size < ELF_MACHINE + 2u ||
        fs_file_open(v, &w->file, &w->ent) != FS_OK ||
        fs_file_read(&w->file, 0, ELF_MACHINE + 2u, w->head) != FS_OK)
        return BIOSBOOT_UNKNOWN;
    return biosboot_elf_is_64(w->head, ELF_MACHINE + 2u);
}

/* Nonzero when an earlier entry named the same kernel, which was judged
 * then: live media list one kernel under several entries. */
static int same_path(const char *a, const char *b)
{
    size_t k;

    for (k = 0; a[k] != '\0' && a[k] == b[k]; k++)
        ;
    return a[k] == b[k];
}

static int seen_before(const bootcfg_result *cfg, size_t i)
{
    size_t j;

    for (j = 0; j < i; j++) {
        if (bootcfg_entry_bootable(&cfg->entry[j]) &&
            !cfg->entry[j].kernel_unresolved && !cfg->entry[j].truncated &&
            same_path(cfg->entry[j].kernel, cfg->entry[i].kernel))
            return 1;
    }
    return 0;
}

/* Every kernel the CD configuration names, judged once each.
 *
 * *direct: YES when one is a bzImage the direct route could be handed, NO
 * when there is provably none (no configuration, or every kernel read and
 * none is one), UNKNOWN otherwise.
 * *needs64: YES when every kernel read and every one is a 64-bit bzImage,
 * NO when one is 32-bit, UNKNOWN otherwise. */
static void judge_kernels(medium *m, biosboot_work *w, char *scratch,
                          size_t scratch_len, biosboot_answer *direct,
                          biosboot_answer *needs64)
{
    bootcfg_src src;
    size_t i;
    int rc;
    int unknown = 0;
    int any_bz = 0;
    int any_32 = 0;
    int any_64 = 0;
    int any_unsized = 0;

    *direct = BIOSBOOT_UNKNOWN;
    *needs64 = BIOSBOOT_UNKNOWN;
    /* The volume falcon-bios's direct route reads the configuration from:
     * the medium's first (falcon-bios/src/iso/scan.c, open_medium). */
    if (medium_count(m) == 0)
        return;
    bootfs_src(&w->bctx, medium_vol_at(m, 0), &src);
    rc = bootcfg_read(&src, BOOTCFG_CD, scratch, scratch_len, &w->cfg);
    if (rc != BOOTCFG_OK && rc != BOOTCFG_ERR_NONE)
        return;
    if (w->cfg.n == 0) {
        /* No entry at all: nothing for the direct route to start, and no
         * kernel whose width could be asked. */
        if (rc == BOOTCFG_ERR_NONE)
            *direct = BIOSBOOT_NO;
        return;
    }

    for (i = 0; i < w->cfg.n && i < BOOTCFG_ENTRIES_MAX; i++) {
        const bootcfg_entry *e = &w->cfg.entry[i];
        uint64_t size = 0;
        uint64_t want;

        /* A memory tester or a .c32 module beside the OS is not what the
         * medium boots, and the direct route never picks one. */
        if (!bootcfg_entry_bootable(e))
            continue;
        if (e->kernel_unresolved || e->truncated) {
            unknown = 1;
            continue;
        }
        if (seen_before(&w->cfg, i))
            continue;
        if (bootfs_open(&w->bctx, e->kernel, &w->file, &size) != FS_OK) {
            unknown = 1;
            continue;
        }
        want = size < (uint64_t)sizeof w->head ? size
                                               : (uint64_t)sizeof w->head;
        if (want != 0 && fs_file_read(&w->file, 0, want, w->head) != FS_OK) {
            unknown = 1;
            continue;
        }
        switch (biosboot_kernel_kind(w->head, (size_t)want, size)) {
        case BIOSBOOT_KERNEL_64:
            any_bz = 1;
            any_64 = 1;
            break;
        case BIOSBOOT_KERNEL_32:
            any_bz = 1;
            any_32 = 1;
            break;
        case BIOSBOOT_KERNEL_OLD:
            any_bz = 1;
            any_unsized = 1;
            break;
        case BIOSBOOT_KERNEL_NONE:
            /* Not a kernel the direct route takes, and not one whose width
             * can be read: memtest86+ before v6, a multiboot image. Whether
             * it needs long mode is not provable. */
            any_unsized = 1;
            break;
        case BIOSBOOT_KERNEL_UNKNOWN:
        default:
            unknown = 1;
            break;
        }
    }

    if (any_bz)
        *direct = BIOSBOOT_YES;
    else if (!unknown)
        *direct = BIOSBOOT_NO;

    if (any_32)
        *needs64 = BIOSBOOT_NO;
    else if (any_64 && !unknown && !any_unsized)
        *needs64 = BIOSBOOT_YES;
}

/* Whether the medium shows a way in for UEFI: an El Torito entry for platform
 * 0xEF, or the removable-media loader or Windows' boot manager on one of its
 * volumes (the evidence media/uefiboot.c takes). {EFI ONLY} says the image
 * boots on UEFI, so a medium BIOS cannot start is marked only when this
 * holds; a data disc that neither can start is left unmarked, as the UEFI
 * menu leaves it. */
static int uefi_way(medium *m, biosboot_work *w)
{
    static const char *const loaders[] = {
        "/EFI/BOOT/BOOTX64.EFI",
        "/bootmgr.efi",
    };
    elt_catalog cat;
    unsigned i, j;

    if (elt_parse(&cat, m->read, m->ctx, m->size) == ELT_OK) {
        for (i = 0; i < cat.n; i++) {
            if (cat.entry[i].platform == ELT_PLATFORM_UEFI)
                return 1;
        }
    }
    for (i = 0; i < medium_count(m); i++) {
        fs_vol *v = medium_vol_at(m, i);

        for (j = 0; j < sizeof loaders / sizeof loaders[0]; j++) {
            if (fs_lookup(v, loaders[j], &w->walk, &w->ent) == FS_OK &&
                !w->ent.is_dir)
                return 1;
        }
    }
    return 0;
}

void biosboot_medium(medium *m, family_id fam, biosboot_work *w,
                     char *scratch, size_t scratch_len, biosboot_result *out)
{
    biosboot_answer image;
    biosboot_answer direct = BIOSBOOT_UNKNOWN;
    biosboot_answer wide = BIOSBOOT_UNKNOWN;

    if (out == (biosboot_result *)0)
        return;
    out->boots = BIOSBOOT_UNKNOWN;
    out->needs64 = BIOSBOOT_UNKNOWN;
    if (m == (medium *)0 || w == (biosboot_work *)0 ||
        scratch == (char *)0 || medium_count(m) == 0)
        return;

    image = biosboot_has_image(m);
    if (fam == FAMILY_WINDOWS) {
        /* No kernel for the direct route, and a width that is boot.wim's. */
        out->boots = image;
        if (out->boots == BIOSBOOT_NO && !uefi_way(m, w))
            out->boots = BIOSBOOT_UNKNOWN;
        return;
    }
    if (fam == FAMILY_BSD) {
        /* The BSD route boots the medium's own loader, so its width is the
         * kernel's ELF header; the configuration is read only for the direct
         * route, which the BSD route falls through to. */
        biosboot_answer ignored;

        wide = bsd_needs64(m, w);
        if (image != BIOSBOOT_YES)
            judge_kernels(m, w, scratch, scratch_len, &direct, &ignored);
    } else {
        /* Read even when the boot image already answers the first question,
         * because the second is asked of its kernels. */
        judge_kernels(m, w, scratch, scratch_len, &direct, &wide);
    }

    if (image == BIOSBOOT_YES || direct == BIOSBOOT_YES)
        out->boots = BIOSBOOT_YES;
    else if (image == BIOSBOOT_NO && direct == BIOSBOOT_NO && uefi_way(m, w))
        out->boots = BIOSBOOT_NO;
    /* A medium nothing here can start needs no processor at all. */
    out->needs64 = out->boots == BIOSBOOT_NO ? BIOSBOOT_UNKNOWN : wide;
}
