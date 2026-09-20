/* The volume set of a medium. See falcon-core/src/media/medium.h for the
 * precedence rule and why this object cannot be copied. */

#include <media/medium.h>

#include <media/eltorito.h>
#include <fs/iso9660.h>
#include <fs/udf.h>
#include <lib/string.h>

/* Every volume reads through this, which is the whole of the per-platform
 * boundary: add a base offset and forward. */
int medium_src_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    const medium_src *s = ctx;

    /* The addition is checked rather than assumed. base comes from an El
     * Torito catalog, which is medium data, and a base near the top of the
     * range plus a large read would otherwise wrap and read from the start
     * of the medium instead of failing. */
    if (off > ~(uint64_t)0 - s->base)
        return -1;
    return s->parent(s->parent_ctx, s->base + off, len, buf);
}

/* Mounts `ops` at `base` into the next free slot, and keeps it only if the
 * driver accepts the volume.
 *
 * The shim is initialised before the mount because the driver reads through
 * it during the mount, which is also why the slot is filled in place rather
 * than built on the stack and copied in: the address handed to fs_mount has
 * to be the one the volume keeps. */
static int add_vol(medium *m, const fs_ops *ops, uint64_t base,
                   medium_vol_kind kind)
{
    medium_vol *v;

    if (m->n >= MEDIUM_MAX_VOLS)
        return 0;
    v = &m->vol[m->n];
    memset(v, 0, sizeof(*v));
    v->src.parent = m->read;
    v->src.parent_ctx = m->ctx;
    v->src.base = base;
    if (fs_mount(&v->vol, ops, medium_src_read, &v->src) != FS_OK) {
        memset(v, 0, sizeof(*v));
        return 0;
    }
    v->ops = ops;
    v->base = base;
    v->kind = kind;
    m->n++;
    return 1;
}

/* A probe-then-mount for one named driver at an offset, so a driver that
 * declines the magic costs one read rather than a full mount attempt. */
static int try_vol(medium *m, const fs_ops *ops, uint64_t base,
                   medium_vol_kind kind)
{
    medium_src probe;

    probe.parent = m->read;
    probe.parent_ctx = m->ctx;
    probe.base = base;
    if (ops->probe != (int (*)(fs_read_fn, void *))0 &&
        ops->probe(medium_src_read, &probe) != FS_OK)
        return 0;
    return add_vol(m, ops, base, kind);
}

/* The El Torito boot images, mounted with whatever driver the registry
 * identifies.
 *
 * fs_identify rather than naming FAT, even though it is FAT on all 36 corpus
 * media: what the boot image holds is the medium's choice, and a consumer
 * asking for /EFI/BOOT/BOOTX64.EFI does not care which driver answers.
 *
 * Every section entry is tried, in catalog order, and the platform ID is not
 * consulted. That is measured rather than lazy: OPNsense 25.1 declares its
 * UEFI FAT boot image with platform 0x00, so selecting on 0xEF would find no
 * boot volume on that medium at all (falcon-core/src/media/eltorito.h). */
static void add_boot_volumes(medium *m)
{
    elt_catalog cat;
    unsigned i;

    if (elt_parse(&cat, m->read, m->ctx, m->size) != ELT_OK)
        return;
    for (i = 0; i < cat.n && m->n < MEDIUM_MAX_VOLS; i++) {
        uint64_t base = (uint64_t)cat.entry[i].lba * ELT_SECTOR;
        medium_src probe;
        const fs_ops *ops;

        /* The default entry's image is raw code loaded at 0x7C00 (isolinux,
         * etfsboot.com), not a filesystem. Under BIOS it is exactly the thing
         * to jump to; here it is a volume that would not mount, and skipping
         * it saves every medium a pointless identify. */
        if (cat.entry[i].is_default)
            continue;
        if (m->size != 0 && base >= m->size)
            continue;
        probe.parent = m->read;
        probe.parent_ctx = m->ctx;
        probe.base = base;
        ops = fs_identify(medium_src_read, &probe, FS_HINT_NONE);
        if (ops == (const fs_ops *)0)
            continue;
        (void)add_vol(m, ops, base, MEDIUM_VOL_BOOT);
    }
}

int medium_open(medium *m, fs_read_fn read, void *ctx, uint64_t size)
{
    unsigned i;

    if (m == (medium *)0)
        return MEDIUM_ERR_ARG;
    memset(m, 0, sizeof(*m));
    if (read == (fs_read_fn)0)
        return MEDIUM_ERR_ARG;
    m->read = read;
    m->ctx = ctx;
    m->size = size;
    m->boot = 0;

    /* Precedence, and the order a consumer should walk them: UDF, then
     * ISO9660, then the boot image. medium.h says why. */
    (void)try_vol(m, &fs_ops_udf, 0, MEDIUM_VOL_UDF);
    (void)try_vol(m, &fs_ops_iso9660, 0, MEDIUM_VOL_ISO9660);
    add_boot_volumes(m);

    /* Not an ISO at all: a .img holding one filesystem, which is what the
     * disk flavour of the virtual medium serves. Only asked when neither
     * name space nor a catalog turned anything up, so an ISO never pays for
     * it. */
    if (m->n == 0) {
        medium_src probe;
        const fs_ops *ops;

        probe.parent = read;
        probe.parent_ctx = ctx;
        probe.base = 0;
        ops = fs_identify(medium_src_read, &probe, FS_HINT_NONE);
        if (ops != (const fs_ops *)0)
            (void)add_vol(m, ops, 0, MEDIUM_VOL_WHOLE);
    }

    /* boot is an index rather than a flag, and n when there is none, so a
     * caller can compare it against medium_count without a second question. */
    m->boot = m->n;
    for (i = 0; i < m->n; i++) {
        if (m->vol[i].kind == MEDIUM_VOL_BOOT) {
            m->boot = i;
            break;
        }
    }
    return m->n > 0 ? MEDIUM_OK : MEDIUM_ERR_NOTHING;
}

unsigned medium_count(const medium *m)
{
    return m == (const medium *)0 ? 0u : m->n;
}

fs_vol *medium_vol_at(medium *m, unsigned i)
{
    if (m == (medium *)0 || i >= m->n)
        return (fs_vol *)0;
    return &m->vol[i].vol;
}

medium_vol_kind medium_kind_at(const medium *m, unsigned i)
{
    if (m == (const medium *)0 || i >= m->n)
        return MEDIUM_VOL_WHOLE;
    return m->vol[i].kind;
}

uint64_t medium_base_at(const medium *m, unsigned i)
{
    if (m == (const medium *)0 || i >= m->n)
        return 0;
    return m->vol[i].base;
}

fs_vol *medium_boot_vol(medium *m)
{
    if (m == (medium *)0 || m->boot >= m->n)
        return (fs_vol *)0;
    return &m->vol[m->boot].vol;
}
