/* falcon-hook-mkextmap: the extent map the loader would publish for an ISO on
 * p2 of a stick image, for booting falcon-hook in a rig without the loader.
 *
 * Finds p2 as the loaders do, by its record (DESIGN Section 3.1): every
 * partition of the image's table (MBR or GPT) whose volume carries a
 * /.falcon record of role library, read with falcon-core's own readers. It
 * resolves the ISO's runs with the volume's extent map (exFAT or FAT),
 * encodes them with media/extmap.c (the library's install ID, its first
 * LBA), and prints the result in the JSON that virt-fw-vars --set-json
 * takes, so a varstore can carry FalconIsoMap into an OVMF boot that starts
 * the kernel directly (falcon-hook/tools/run-hook-rig.sh).
 *
 * Usage: falcon-hook-mkextmap STICK.img NAME-ON-P2-ROOT
 */

#include <fs/fs.h>
#include <ident/identfs.h>
#include <media/extmap.h>
#include <part/table.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fd;
static uint64_t g_base;

static int rd_disk(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    (void)ctx;
    return pread(g_fd, buf, (size_t)len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

static int rd_part(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    return rd_disk(ctx, g_base + off, len, buf);
}

static part_entry g_parts[128];
static ident_fs_scratch g_rec;
static fs_dir g_dir;
static fs_extent g_ext[EXTMAP_MAX_EXTENTS];
static extmap_extent g_map_ext[EXTMAP_MAX_EXTENTS];
static uint8_t g_buf[EXTMAP_HDR_LEN + EXTMAP_MAX_EXTENTS * EXTMAP_EXT_LEN];

int main(int argc, char **argv)
{
    part_table t;
    const part_entry *p2 = NULL;
    ident_rec rec;
    fs_dirent e;
    extmap_hdr h;
    size_t n = 0, len = 0, i;
    uint32_t k;
    int found = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s STICK.img NAME-ON-P2-ROOT\n", argv[0]);
        return 2;
    }
    memset(&t, 0, sizeof t);
    t.parts = g_parts;
    t.cap = 128;
    g_fd = open(argv[1], O_RDONLY);
    if (g_fd < 0 || part_read_table(&t, rd_disk, NULL, 512, 0) != PART_OK) {
        fprintf(stderr, "%s: no partition table\n", argv[1]);
        return 1;
    }
    for (k = 0; k < t.count && p2 == NULL; k++) {
        g_base = g_parts[k].start_lba * 512u;
        if (ident_probe(rd_part, NULL, &g_rec, &rec) == IDENT_OK &&
            rec.role == IDENT_ROLE_LIBRARY)
            p2 = &g_parts[k];
    }
    if (p2 == NULL) {
        fprintf(stderr, "%s: no partition carries a library record\n",
                argv[1]);
        return 1;
    }
    /* g_base is p2's, and g_rec.vol is p2 mounted. */
    if (!fs_has_extents(&g_rec.vol) ||
        fs_dir_open_root(&g_rec.vol, &g_dir) != FS_OK) {
        fprintf(stderr, "%s: p2 is not a volume whose files map to extents\n",
                argv[1]);
        return 1;
    }
    while (fs_dir_next(&g_dir, &e) == 1) {
        if (fs_name_eq_ascii(e.name, argv[2])) {
            found = 1;
            break;
        }
    }
    if (!found ||
        fs_extents(&g_rec.vol, &e, g_ext, EXTMAP_MAX_EXTENTS, &n) != FS_OK) {
        fprintf(stderr, "%s: no such file on p2, or too fragmented\n", argv[2]);
        return 1;
    }
    memset(&h, 0, sizeof h);
    memcpy(h.install_id, rec.install_id, 16);
    h.part_lba = p2->start_lba;
    h.block_size = 512;
    h.n_extents = (uint32_t)n;
    h.iso_size = e.size;
    for (i = 0; i < n; i++) {
        g_map_ext[i].offset = g_ext[i].vol_off;
        g_map_ext[i].length = g_ext[i].len;
    }
    if (extmap_encode(&h, g_map_ext, g_buf, sizeof g_buf, &len) != EXTMAP_OK) {
        fprintf(stderr, "the extents do not make a map\n");
        return 1;
    }
    printf("{\"version\":2,\"variables\":[{\"name\":\"" FALCON_EXTMAP_VAR
           "\",\"guid\":\"" FALCON_EXTMAP_GUID_STR "\",\"attr\":7,\"data\":\"");
    for (i = 0; i < len; i++)
        printf("%02x", g_buf[i]);
    printf("\"}]}\n");
    fprintf(stderr, "%zu extent(s), %llu bytes, p2 at LBA %llu\n", n,
            (unsigned long long)e.size, (unsigned long long)p2->start_lba);
    return 0;
}
