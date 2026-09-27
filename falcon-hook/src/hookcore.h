/* falcon-hook: the decisions, apart from the system calls that act on them.
 *
 * Everything here is pure: parsing /proc/cmdline, reading the efivarfs file,
 * choosing which block devices to look at, and building the device-mapper
 * table from the extent map. It is what the host tests exercise; hook.c is
 * the sequence of mounts, reads and ioctls around it.
 */

#ifndef FALCON_HOOKCORE_H
#define FALCON_HOOKCORE_H

#include <stddef.h>
#include <stdint.h>

#include <media/extmap.h>

/* The device-mapper device the hook creates. The name is what
 * /dev/mapper/ shows; the uuid is what dm-aware udev rules key on. */
#define HOOK_DM_NAME "falcon-iso"
#define HOOK_DM_UUID "FALCON-ISO"

/* Where the distro's own init is, unless the cmdline says otherwise. */
#define HOOK_DEFAULT_INIT "/init"

/* The argument that carries the distro's own rdinit= when the loader had to
 * replace it with ours. */
#define HOOK_INIT_ARG "falcon.init"

/* The value of key= in a kernel cmdline, into out. 1 when found and it fit,
 * 0 otherwise. The last occurrence wins, as the kernel's own parser has it.
 * Quoted values ("key=\"a b\"") are unquoted. */
int hook_cmdline_arg(const char *cmdline, size_t len, const char *key,
                     char *out, size_t cap);

/* efivarfs prefixes a variable's data with its 4-byte attributes. The data,
 * or NULL when the file is too short to hold any. */
const uint8_t *hook_efivar_data(const uint8_t *file, size_t len,
                                size_t *data_len);

/* Is a /sys/block name one that could be the stick? Whole disks only, and
 * none of the virtual kinds: loop, ram, zram, dm, md, nbd, sr (a CD cannot
 * hold a GPT that names p2), fd. */
int hook_disk_candidate(const char *name);

/* Parses "MAJ:MIN\n" as /sys/block/X/dev has it. 1 on success. */
int hook_parse_devnum(const char *s, size_t len, uint32_t *maj, uint32_t *min);

/* The DM_TABLE_LOAD argument: a struct dm_ioctl for HOOK_DM_NAME, read-only,
 * followed by one linear target per extent, mapping the ISO's bytes in order
 * onto the block device maj:min at base + extent offset. base is 0 when
 * maj:min is the partition itself, which is how the hook maps: claiming the
 * partition leaves the rest of the stick usable, where claiming the whole
 * disk would make every partition on it unmountable. Mapping the whole disk
 * (base = the partition's byte offset) is the fallback for a kernel that did
 * not create partition devices.
 *
 * HOOK_ERR_RANGE when an extent or base is not whole blocks of the map's
 * block_size (512 when it names none; every extent but the last must be; the
 * last is rounded up to a block, which is safe because the tail of its last
 * cluster belongs to the file). Whole blocks rather than 512-byte sectors
 * because the kernel loads no table misaligned to the disk's logical block,
 * which on a 4Kn disk is 4096. */
enum {
    HOOK_OK          = 0,
    HOOK_ERR_BAD_ARG = -1,
    HOOK_ERR_NOSPACE = -2,
    HOOK_ERR_RANGE   = -3
};

int hook_dm_table(const extmap_hdr *hdr, const uint8_t *map, uint32_t maj,
                  uint32_t min, uint64_t base, void *buf, size_t cap,
                  size_t *len);

/* The kernel's dev_t encoding of maj:min, as mknod takes it. */
unsigned long hook_devnum(uint32_t maj, uint32_t min);

/* A bare struct dm_ioctl for HOOK_DM_NAME in buf, for the commands that
 * carry no payload (version, create, resume). */
int hook_dm_header(void *buf, size_t cap, int with_uuid);

/* The names udev would give an ISO9660 filesystem, from its Primary Volume
 * Descriptor (the 2048 bytes at offset 32768): the volume label, encoded as
 * udev encodes a by-label link name (anything outside [0-9A-Za-z#+-.:=@_]
 * as \xHH, so a space is \x20), and the "UUID" blkid derives from the
 * volume's modification date, or its creation date when that is unset, as
 * YYYY-MM-DD-HH-mm-ss-cc. Either is left empty when the descriptor has
 * none. 1 when pvd is a primary volume descriptor at all. */
int hook_iso_names(const uint8_t *pvd, char *label, size_t label_cap,
                   char *uuid, size_t uuid_cap);

/* Bytes hook_dm_table needs for n extents. */
size_t hook_dm_table_size(uint32_t n_extents);

/* ---- nudges (docs/notes/coverage-roadmap.md M4) ----
 *
 * Some initramfs scripts look for their media by a list of device names that
 * a device-mapper device is not on. Where the script can be taught with one
 * textual change, the hook makes it in the initramfs (writable, and not yet
 * running) before handing over: the kind of per-family hook Ventoy carries,
 * kept to a table of exact substitutions so each one is visible and tested.
 *
 * A nudge applies when path exists and contains find; find is replaced once
 * by replace. A file that already contains replace is left alone. */
typedef struct {
    const char *what;      /* for the log */
    const char *path;      /* in the initramfs */
    const char *find;
    const char *replace;
} hook_nudge;

/* The table, and its length. */
const hook_nudge *hook_nudges(size_t *count);

/* An initramfs that looks for its medium mounted at a fixed place first
 * (the Debian installer's cdrom-detect: "Is a cdrom already mounted? If so,
 * assume it's the right one"). When marker exists, the hook mounts the ISO
 * there, read-only, before handing over. */
typedef struct {
    const char *what;
    const char *marker;    /* a file only that initramfs has */
    const char *dir;       /* where it looks */
} hook_premount;

const hook_premount *hook_premounts(size_t *count);

/* The package that carries a missing block driver, for an initramfs that
 * can unpack the distro's own packages (the Debian installer's udpkg): its
 * file name is prefix, the running kernel's release, then middle, and it
 * ends in suffix. 1 when name is one, for that release. */
int hook_module_package(const char *name, const char *release,
                        const char *prefix);

/* One substitution: text (len bytes) with the first find replaced by
 * replace, into out. 1 when it was made, 0 when find is absent or replace is
 * already present (nothing to do), HOOK_ERR_NOSPACE when out is short. */
int hook_patch(const char *text, size_t len, const char *find,
               const char *replace, char *out, size_t cap, size_t *out_len);

#endif /* FALCON_HOOKCORE_H */
