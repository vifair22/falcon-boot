#include "synth.h"

#include <stdlib.h>
#include <string.h>

void synth_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void synth_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void synth_put_le64(uint8_t *p, uint64_t v)
{
    synth_put_le32(p, (uint32_t)v);
    synth_put_le32(p + 4, (uint32_t)(v >> 32));
}

uint32_t synth_cluster_bytes(const synth_vol *sv)
{
    return 1u << (sv->bps_shift + sv->spc_shift);
}

void synth_fat_set(synth_vol *sv, uint32_t cl, uint32_t val)
{
    uint64_t off = ((uint64_t)sv->fat_offset << sv->bps_shift) +
                   (uint64_t)cl * 4u;
    synth_put_le32(sv->buf + off, val);
}

uint8_t *synth_cluster(synth_vol *sv, uint32_t cl)
{
    return sv->buf + ((uint64_t)sv->heap_offset << sv->bps_shift) +
           ((uint64_t)(cl - 2u) << (sv->bps_shift + sv->spc_shift));
}

void synth_init(synth_vol *sv, unsigned bps_shift, unsigned spc_shift,
                uint32_t cluster_count)
{
    uint32_t ss = 1u << bps_shift;
    uint32_t fat_bytes = (cluster_count + 2u) * 4u;
    uint32_t i;

    memset(sv, 0, sizeof(*sv));
    sv->bps_shift = bps_shift;
    sv->spc_shift = spc_shift;
    sv->cluster_count = cluster_count;
    sv->root_cluster = 2;
    /* Sector 0 is the VBR; the spec's full boot region is 24 sectors, but the
     * driver reads nothing between the VBR and the FAT, so two is enough and
     * keeps the buffers small. */
    sv->fat_offset = 2;
    sv->fat_sectors = (fat_bytes + ss - 1u) / ss;
    sv->heap_offset = sv->fat_offset + sv->fat_sectors;
    sv->fail_off = -1;

    sv->len = ((uint64_t)sv->heap_offset << bps_shift) +
              ((uint64_t)cluster_count << (bps_shift + spc_shift));
    sv->buf = calloc(1, (size_t)sv->len);
    if (sv->buf == NULL)
        abort();

    sv->buf[0] = 0xEB;
    sv->buf[1] = 0x76;
    sv->buf[2] = 0x90;
    memcpy(sv->buf + 3, "EXFAT   ", 8);
    synth_put_le64(sv->buf + 72, sv->len >> bps_shift); /* VolumeLength */
    synth_put_le32(sv->buf + 80, sv->fat_offset);
    synth_put_le32(sv->buf + 84, sv->fat_sectors);
    synth_put_le32(sv->buf + 88, sv->heap_offset);
    synth_put_le32(sv->buf + 92, cluster_count);
    synth_put_le32(sv->buf + 96, sv->root_cluster);
    synth_put_le16(sv->buf + 104, 0x0100);              /* FileSystemRevision */
    sv->buf[108] = (uint8_t)bps_shift;
    sv->buf[109] = (uint8_t)spc_shift;
    sv->buf[110] = 1;                                   /* NumberOfFats */
    sv->buf[510] = 0x55;
    sv->buf[511] = 0xAA;

    for (i = 2; i < cluster_count + 2u; i++)
        synth_fat_set(sv, i, 0xFFFFFFFFu);
}

void synth_free(synth_vol *sv)
{
    free(sv->buf);
    sv->buf = NULL;
}

int synth_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    synth_vol *sv = ctx;

    sv->reads++;
    if (sv->fail_on_read != 0 && sv->reads >= sv->fail_on_read)
        return -1;
    if (sv->fail_off >= 0 && off == (uint64_t)sv->fail_off)
        return -1;
    if (off > sv->len || len > sv->len - off)
        return -1;
    memcpy(buf, sv->buf + off, (size_t)len);
    return 0;
}

void synth_put_file(uint8_t *p, uint8_t sec_count, uint16_t attrs)
{
    memset(p, 0, SYNTH_ENT);
    p[0] = 0x85;
    p[1] = sec_count;
    synth_put_le16(p + 4, attrs);
}

void synth_put_stream(uint8_t *p, uint8_t flags, uint8_t name_len,
                      uint32_t first_cluster, uint64_t valid_len,
                      uint64_t data_len)
{
    memset(p, 0, SYNTH_ENT);
    p[0] = 0xC0;
    p[1] = flags;
    p[3] = name_len;
    synth_put_le64(p + 8, valid_len);
    synth_put_le32(p + 20, first_cluster);
    synth_put_le64(p + 24, data_len);
}

void synth_put_name(uint8_t *p, const char *s, size_t n)
{
    size_t i;

    memset(p, 0, SYNTH_ENT);
    p[0] = 0xC1;
    for (i = 0; i < n && i < 15; i++)
        synth_put_le16(p + 2 + 2 * i, (uint16_t)(unsigned char)s[i]);
}

void synth_put_end(uint8_t *p)
{
    memset(p, 0, SYNTH_ENT);
}

unsigned synth_put_set(uint8_t *p, const char *name, uint16_t attrs,
                       uint8_t flags, uint32_t first_cluster, uint64_t size)
{
    size_t nl = strlen(name);
    unsigned name_ents = (unsigned)((nl + 14) / 15);
    unsigned i;

    if (name_ents == 0)
        name_ents = 1; /* the spec always allocates at least one name entry */

    synth_put_file(p, (uint8_t)(1u + name_ents), attrs);
    synth_put_stream(p + SYNTH_ENT, flags, (uint8_t)nl, first_cluster,
                     size, size);
    for (i = 0; i < name_ents; i++) {
        size_t off = (size_t)i * 15;
        size_t take = nl > off ? nl - off : 0;

        if (take > 15)
            take = 15;
        synth_put_name(p + SYNTH_ENT * (2u + i), name + off, take);
    }
    return 2u + name_ents;
}
