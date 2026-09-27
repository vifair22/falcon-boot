/* Can an x86-64 UEFI machine boot this medium. See
 * falcon-core/src/media/uefiboot.h. */

#include <media/uefiboot.h>

#include <media/eltorito.h>
#include <pe/pe.h>
#include <lib/string.h>

/* The Linux x86 boot protocol's setup header magic, "HdrS" at 0x202: the
 * mark of a bzImage whether or not it also carries a PE header. */
#define SETUP_MAGIC_OFF 0x202u

/* What the chainload and the Windows route look for. The first is the
 * removable-media path every UEFI medium uses; the second is Windows' boot
 * manager at the root of its install media, which the Windows route serves
 * even when a medium's own El Torito image is not the way in. */
static const char *const g_loaders[] = {
    "/EFI/BOOT/BOOTX64.EFI",
    "/bootmgr.efi",
};

uefiboot_answer uefiboot_kernel(const void *head, size_t len)
{
    const uint8_t *b = head;
    pe_hdr h;
    int bz;

    if (b == (const uint8_t *)0)
        return UEFIBOOT_UNKNOWN;
    if (pe_parse(b, len, &h) == PE_OK) {
        /* The direct route hands the kernel to LoadImage, and an x86-64
         * firmware loads an x86-64 PE32+ and nothing else. A 32-bit EFI
         * stub (i386, PE32) is the one that looks closest and is still NO:
         * mixed mode runs a 64-bit kernel on 32-bit firmware, never the
         * reverse. */
        if (h.machine == PE_MACHINE_AMD64 && h.magic == PE_MAGIC_PE32P)
            return UEFIBOOT_YES;
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

int uefiboot_has_image(medium *m, uefiboot_work *w)
{
    elt_catalog cat;
    unsigned i, j;

    if (m == (medium *)0 || w == (uefiboot_work *)0)
        return 0;
    /* The platform ID is read here even though medium.c will not select on
     * it (OPNsense declares its UEFI image as x86): an entry that DOES say
     * 0xEF is a UEFI image by the medium's own statement, whatever it holds,
     * and the lookups below catch the ones that say otherwise. */
    if (elt_parse(&cat, m->read, m->ctx, m->size) == ELT_OK) {
        for (i = 0; i < cat.n; i++) {
            if (cat.entry[i].platform == ELT_PLATFORM_UEFI)
                return 1;
        }
    }
    for (i = 0; i < medium_count(m); i++) {
        fs_vol *v = medium_vol_at(m, i);

        for (j = 0; j < sizeof g_loaders / sizeof g_loaders[0]; j++) {
            if (fs_lookup(v, g_loaders[j], &w->walk, &w->ent) == FS_OK &&
                !w->ent.is_dir)
                return 1;
        }
    }
    return 0;
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
                                char *scratch, size_t scratch_len)
{
    bootcfg_src src;
    const bootcfg_entry *e;
    fs_vol *v;
    uint64_t size;
    uint64_t want;

    if (m == (medium *)0 || w == (uefiboot_work *)0 ||
        scratch == (char *)0 || medium_count(m) == 0)
        return UEFIBOOT_UNKNOWN;
    if (uefiboot_has_image(m, w))
        return UEFIBOOT_YES;
    /* No kernel to ask about: Windows boots through its own loader or not
     * at all. */
    if (fam == FAMILY_WINDOWS)
        return UEFIBOOT_NO;

    v = direct_vol(m);
    if (v == (fs_vol *)0)
        return UEFIBOOT_UNKNOWN;
    bootfs_src(&w->bctx, v, &src);
    if (bootcfg_read(&src, BOOTCFG_CD, scratch, scratch_len, &w->cfg) !=
            BOOTCFG_OK ||
        w->cfg.pick >= w->cfg.n)
        return UEFIBOOT_UNKNOWN;
    e = &w->cfg.entry[w->cfg.pick];
    if (bootfs_open(&w->bctx, e->kernel, &w->kernel, &size) != FS_OK ||
        size == 0)
        return UEFIBOOT_UNKNOWN;
    want = size < (uint64_t)sizeof w->head ? size : (uint64_t)sizeof w->head;
    if (fs_file_read(&w->kernel, 0, want, w->head) != FS_OK)
        return UEFIBOOT_UNKNOWN;
    return uefiboot_kernel(w->head, (size_t)want);
}
