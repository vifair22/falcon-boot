/* Can a UEFI machine of a given architecture boot this medium. See
 * falcon-core/src/media/uefiboot.h. */

#include <media/uefiboot.h>

#include <media/eltorito.h>
#include <pe/pe.h>
#include <lib/string.h>

/* The Linux x86 boot protocol's setup header magic, "HdrS" at 0x202: the
 * mark of a bzImage whether or not it also carries a PE header. */
#define SETUP_MAGIC_OFF 0x202u

/* What the chainload and the Windows route look for: the removable-media
 * path every UEFI medium uses, by architecture. Windows' boot manager at the
 * root of its install media, which the Windows route serves even when a
 * medium's own El Torito image is not the way in, is x64's: Windows on ARM
 * media carries BOOTAA64.EFI, which is what counts there. */
static const char *const g_loaders_x64[] = {
    "/EFI/BOOT/BOOTX64.EFI",
    "/bootmgr.efi"
};

static const char *const g_loaders_aa64[] = {
    "/EFI/BOOT/BOOTAA64.EFI"
};

typedef struct {
    const char *const *names;
    unsigned n;
} loader_set;

static loader_set loaders(uint16_t machine)
{
    loader_set s;

    if (machine == PE_MACHINE_ARM64) {
        s.names = g_loaders_aa64;
        s.n = (unsigned)(sizeof g_loaders_aa64 / sizeof g_loaders_aa64[0]);
    } else {
        s.names = g_loaders_x64;
        s.n = (unsigned)(sizeof g_loaders_x64 / sizeof g_loaders_x64[0]);
    }
    return s;
}

/* The architecture whose removable-media loader is not this one's. */
static uint16_t other_machine(uint16_t machine)
{
    return machine == PE_MACHINE_ARM64 ? PE_MACHINE_AMD64 : PE_MACHINE_ARM64;
}

static int is_uefi_machine(uint16_t machine)
{
    return machine == PE_MACHINE_AMD64 || machine == PE_MACHINE_ARM64;
}

uefiboot_answer uefiboot_kernel(const void *head, size_t len,
                                uint16_t machine)
{
    const uint8_t *b = head;
    pe_hdr h;
    int bz;

    if (b == (const uint8_t *)0)
        return UEFIBOOT_UNKNOWN;
    if (pe_parse(b, len, &h) == PE_OK) {
        /* The direct route hands the kernel to LoadImage, and a firmware
         * loads a PE32+ of its own architecture and nothing else. A 32-bit
         * EFI stub (i386, PE32) is the one that looks closest and is still
         * NO: mixed mode runs a 64-bit kernel on 32-bit firmware, never the
         * reverse. The other 64-bit architecture's kernel is OTHER. */
        if (h.magic == PE_MAGIC_PE32P && h.machine == machine)
            return UEFIBOOT_YES;
        if (h.magic == PE_MAGIC_PE32P && is_uefi_machine(h.machine))
            return UEFIBOOT_OTHER;
        return UEFIBOOT_NO;
    }
    /* An "MZ" that did not parse may be a PE whose header is past the window
     * or damaged; either way not something to call BIOS-only. */
    if (len >= 2u && b[0] == 'M' && b[1] == 'Z')
        return UEFIBOOT_UNKNOWN;
    bz = len >= SETUP_MAGIC_OFF + 4u &&
         memcmp(b + SETUP_MAGIC_OFF, "HdrS", 4) == 0;
    return bz ? UEFIBOOT_NO : UEFIBOOT_UNKNOWN;
}

/* Whether any of the medium's volumes carries one of the set's names. */
static int has_loader(medium *m, uefiboot_work *w, loader_set set)
{
    unsigned i, j;

    for (i = 0; i < medium_count(m); i++) {
        fs_vol *v = medium_vol_at(m, i);

        for (j = 0; j < set.n; j++) {
            if (fs_lookup(v, set.names[j], &w->walk, &w->ent) == FS_OK &&
                !w->ent.is_dir)
                return 1;
        }
    }
    return 0;
}

uefiboot_answer uefiboot_has_image(medium *m, uefiboot_work *w,
                                   uint16_t machine)
{
    elt_catalog cat;
    unsigned i;

    if (m == (medium *)0 || w == (uefiboot_work *)0)
        return UEFIBOOT_UNKNOWN;
    if (has_loader(m, w, loaders(machine)))
        return UEFIBOOT_YES;
    if (has_loader(m, w, loaders(other_machine(machine))))
        return UEFIBOOT_OTHER;
    /* AArch64 counts a medium by its loader alone. An x86-64 medium's own
     * loader can sit in an El Torito image these volumes do not reach, so
     * there a 0xEF entry is taken at its word, as it always was. */
    if (machine == PE_MACHINE_ARM64)
        return UEFIBOOT_NO;
    /* The platform ID is read here even though medium.c will not select on
     * it (OPNsense declares its UEFI image as x86): an entry that DOES say
     * 0xEF is a UEFI image by the medium's own statement, whatever it holds,
     * and the lookups below catch the ones that say otherwise. */
    if (elt_parse(&cat, m->read, m->ctx, m->size) == ELT_OK) {
        for (i = 0; i < cat.n; i++) {
            if (cat.entry[i].platform == ELT_PLATFORM_UEFI)
                return UEFIBOOT_YES;
        }
    }
    return UEFIBOOT_NO;
}

/* The name space the direct route reads its configuration and kernel from:
 * ISO9660 first, then UDF, the order falcon-efi/src/direct mounts them in. */
static fs_vol *direct_vol(medium *m)
{
    static const medium_vol_kind order[] = { MEDIUM_VOL_ISO9660,
                                             MEDIUM_VOL_UDF };
    unsigned k, i;

    for (k = 0; k < sizeof order / sizeof order[0]; k++) {
        for (i = 0; i < medium_count(m); i++) {
            if (medium_kind_at(m, i) == order[k])
                return medium_vol_at(m, i);
        }
    }
    return (fs_vol *)0;
}

uefiboot_answer uefiboot_medium(medium *m, family_id fam, uefiboot_work *w,
                                char *scratch, size_t scratch_len,
                                uint16_t machine)
{
    bootcfg_src src;
    const bootcfg_entry *e;
    fs_vol *v;
    uint64_t size;
    uint64_t want;
    uefiboot_answer image;
    uefiboot_answer kernel;

    if (m == (medium *)0 || w == (uefiboot_work *)0 ||
        scratch == (char *)0 || medium_count(m) == 0)
        return UEFIBOOT_UNKNOWN;
    image = uefiboot_has_image(m, w, machine);
    if (image == UEFIBOOT_YES)
        return UEFIBOOT_YES;
    /* No kernel to ask about: Windows boots through its own loader or not
     * at all. */
    if (fam == FAMILY_WINDOWS)
        return image;

    v = direct_vol(m);
    if (v == (fs_vol *)0)
        return image == UEFIBOOT_OTHER ? UEFIBOOT_OTHER : UEFIBOOT_UNKNOWN;
    bootfs_src(&w->bctx, v, &src);
    kernel = UEFIBOOT_UNKNOWN;
    if (bootcfg_read(&src, BOOTCFG_CD, scratch, scratch_len, &w->cfg) ==
            BOOTCFG_OK &&
        w->cfg.pick < w->cfg.n) {
        e = &w->cfg.entry[w->cfg.pick];
        if (bootfs_open(&w->bctx, e->kernel, &w->kernel, &size) == FS_OK &&
            size != 0) {
            want = size < (uint64_t)sizeof w->head ? size
                                                   : (uint64_t)sizeof w->head;
            if (fs_file_read(&w->kernel, 0, want, w->head) == FS_OK)
                kernel = uefiboot_kernel(w->head, (size_t)want, machine);
        }
    }
    /* The other architecture's loader with no kernel this one can start
     * is that architecture's medium, not a BIOS one. */
    if (image == UEFIBOOT_OTHER && kernel != UEFIBOOT_YES)
        return UEFIBOOT_OTHER;
    return kernel;
}
