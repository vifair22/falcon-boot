/* The extent map readers' filesystem registry (media/extfind.h): exFAT, the
 * ISO library's filesystem, and nothing else. falcon-hook, falcon-winjump
 * and falcon-vdisk read one file off one kind of volume, and linking this
 * table instead of fs_reg_all.c keeps every other driver out of them.
 */

#include <fs/fs_adapter.h>

const fs_ops *const fs_registry[] = {
    &fs_ops_exfat
};

const size_t fs_registry_n = sizeof fs_registry / sizeof fs_registry[0];
