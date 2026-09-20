/* falcon-bios: the virtual CD's map. See vcdcore.h. */

#include <vcdcore.h>

int vcd_build(vcd_map *m, const vcd_extent *ext, size_t n_ext,
              uint64_t part_lba, uint32_t sector_size, uint64_t iso_size,
              uint8_t drive, uint8_t phys)
{
    uint64_t served = 0;
    uint32_t cd = 0;
    size_t i;

    if (m == (vcd_map *)0 || ext == (const vcd_extent *)0 ||
        sector_size == 0 || (VCD_CD_SECTOR % sector_size) != 0)
        return VCD_ERR_BAD_ARG;
    if (n_ext == 0 || iso_size == 0)
        return VCD_ERR_EMPTY;

    m->per_cd = VCD_CD_SECTOR / sector_size;
    m->n_runs = 0;
    m->n_over = 0;
    m->drive = drive;
    m->phys = phys;

    for (i = 0; i < n_ext; i++) {
        uint64_t len = ext[i].len;
        uint64_t disk;
        uint32_t n_cd;

        /* The last extent may run past the end of the file, because a
         * filesystem allocates whole clusters. Only the file's own bytes
         * are served. */
        if (served >= iso_size)
            break;
        if (len > iso_size - served)
            len = iso_size - served;

        /* Every run has to start and end on a CD sector, or the handler
         * would have to split a read across two of them and the arithmetic
         * stops being one multiply. exFAT's clusters are 4 KB and up, so
         * this holds for any ISO copied onto p2; an ISO on a filesystem
         * with smaller allocation would not serve, and saying so is better
         * than serving it wrong. */
        if ((ext[i].off % VCD_CD_SECTOR) != 0)
            return VCD_ERR_ALIGN;
        if ((len % VCD_CD_SECTOR) != 0 && served + len < iso_size)
            return VCD_ERR_ALIGN;

        disk = part_lba + ext[i].off / sector_size;
        n_cd = (uint32_t)((len + VCD_CD_SECTOR - 1u) / VCD_CD_SECTOR);
        if (n_cd == 0) {
            served += len;
            continue;
        }

        /* Merge with the run before it when the disk addresses join up,
         * which is the common case: an ISO copied onto a fresh partition is
         * many extents and one run. */
        if (m->n_runs > 0) {
            vcd_run *prev = &m->run[m->n_runs - 1u];

            if (prev->disk_lba + (uint64_t)prev->n_cd * m->per_cd == disk) {
                prev->n_cd += n_cd;
                served += len;
                cd += n_cd;
                continue;
            }
        }
        if (m->n_runs >= VCD_MAX_RUNS)
            return VCD_ERR_TOO_MANY;
        m->run[m->n_runs].cd_lba = cd;
        m->run[m->n_runs].n_cd = n_cd;
        m->run[m->n_runs].disk_lba = disk;
        m->n_runs++;
        served += len;
        cd += n_cd;
    }

    if (m->n_runs == 0)
        return VCD_ERR_EMPTY;
    m->n_real = cd;
    m->n_cd = (cd + VCD_PAD - 1u) / VCD_PAD * VCD_PAD;
    m->n_limit = m->n_cd + VCD_PAD;
    return VCD_OK;
}

int vcd_locate(const vcd_map *m, uint32_t cd_lba, uint32_t want,
               uint64_t *disk_lba, uint32_t *n_cd)
{
    uint32_t i;

    if (m == (const vcd_map *)0 || disk_lba == (uint64_t *)0 ||
        n_cd == (uint32_t *)0 || want == 0)
        return VCD_ERR_BAD_ARG;
    if (cd_lba >= m->n_real)
        return VCD_ERR_BAD_ARG;

    for (i = 0; i < m->n_runs; i++) {
        const vcd_run *r = &m->run[i];
        uint32_t into;

        if (cd_lba < r->cd_lba || cd_lba >= r->cd_lba + r->n_cd)
            continue;
        into = cd_lba - r->cd_lba;
        *disk_lba = r->disk_lba + (uint64_t)into * m->per_cd;
        *n_cd = r->n_cd - into;
        if (*n_cd > want)
            *n_cd = want;
        return VCD_OK;
    }
    return VCD_ERR_BAD_ARG;
}

static int override_add(vcd_map *m, uint32_t cd_lba, uint32_t n_cd,
                        uint16_t seg, uint16_t flags, uint32_t phys)
{
    uint32_t i;

    if (m == (vcd_map *)0 || n_cd == 0)
        return VCD_ERR_BAD_ARG;
    if (cd_lba >= m->n_real || n_cd > m->n_real - cd_lba)
        return VCD_ERR_BAD_ARG;
    for (i = 0; i < m->n_over; i++) {
        /* Two stretches of the same medium served from two places is not a
         * thing the handler can answer, so it is refused here where there is
         * somewhere to say so. */
        if (cd_lba < m->over[i].cd_lba + m->over[i].n_cd &&
            m->over[i].cd_lba < cd_lba + n_cd)
            return VCD_ERR_BAD_ARG;
    }
    if (m->n_over >= VCD_MAX_OVERRIDES)
        return VCD_ERR_TOO_MANY;
    m->over[m->n_over].cd_lba = cd_lba;
    m->over[m->n_over].n_cd = n_cd;
    m->over[m->n_over].seg = seg;
    m->over[m->n_over].flags = flags;
    m->over[m->n_over].phys = phys;
    m->n_over++;
    return VCD_OK;
}

int vcd_override_add(vcd_map *m, uint32_t cd_lba, uint32_t n_cd, uint16_t seg)
{
    return override_add(m, cd_lba, n_cd, seg, 0, 0);
}

int vcd_override_add_ext(vcd_map *m, uint32_t cd_lba, uint32_t n_cd,
                         uint32_t phys)
{
    return override_add(m, cd_lba, n_cd, 0, (uint16_t)VCD_OVER_EXT, phys);
}

int vcd_extend(vcd_map *m, uint32_t extra)
{
    if (m == (vcd_map *)0 || extra == 0)
        return VCD_ERR_BAD_ARG;
    if (extra > 0xFFFFFFFFu - m->n_real)
        return VCD_ERR_BAD_ARG;
    m->n_real += extra;
    m->n_cd = (m->n_real + VCD_PAD - 1u) / VCD_PAD * VCD_PAD;
    m->n_limit = m->n_cd + VCD_PAD;
    return VCD_OK;
}

int vcd_span(const vcd_map *m, uint32_t cd_lba, uint32_t want,
             uint16_t *seg, uint32_t *phys, uint32_t *n_cd)
{
    uint32_t gap;
    uint32_t i;

    if (m == (const vcd_map *)0 || seg == (uint16_t *)0 ||
        phys == (uint32_t *)0 || n_cd == (uint32_t *)0 || want == 0)
        return VCD_ERR_BAD_ARG;
    *phys = 0;
    if (cd_lba >= m->n_limit)
        return VCD_ERR_BAD_ARG;

    /* The tail padding, which no run and no override covers. */
    if (cd_lba >= m->n_real) {
        uint32_t have = m->n_limit - cd_lba;

        *seg = 0;
        *n_cd = have < want ? have : want;
        return VCD_FROM_ZERO;
    }

    gap = want;
    for (i = 0; i < m->n_over; i++) {
        const vcd_override *o = &m->over[i];

        if (cd_lba >= o->cd_lba && cd_lba < o->cd_lba + o->n_cd) {
            uint32_t into = cd_lba - o->cd_lba;
            uint32_t have = o->n_cd - into;

            *n_cd = have < want ? have : want;
            if ((o->flags & VCD_OVER_EXT) != 0) {
                *phys = o->phys + into * VCD_CD_SECTOR;
                return VCD_FROM_EXTENDED;
            }
            *seg = (uint16_t)(o->seg +
                              (uint16_t)(into * (VCD_CD_SECTOR / 16u)));
            return VCD_FROM_MEMORY;
        }
        /* Not in it, but possibly in the way: a disk read may not run into
         * an override, because the handler issues one BIOS call per pass. */
        if (o->cd_lba > cd_lba && o->cd_lba - cd_lba < gap)
            gap = o->cd_lba - cd_lba;
    }
    /* And a disk read stops at the padding as it stops at an override. */
    if (m->n_real - cd_lba < gap)
        gap = m->n_real - cd_lba;
    *seg = 0;
    *n_cd = gap;
    return VCD_OK;
}
