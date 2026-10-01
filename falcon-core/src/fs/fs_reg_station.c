/* The station's filesystem registry (DESIGN Section 12): ext4, the shelf's
 * filesystem, and FAT, the station partition's (STATION.INI and
 * MEMTEST.BIN on BIOS) and the ESP's. ext4 first, so a shelf is asked for
 * by its own driver before FAT's probe runs over it. As with fs_reg_ext4.c,
 * linking this table instead of fs_reg_all.c is what keeps the other
 * drivers out of the build.
 */

#include <fs/fs_adapter.h>

const fs_ops *const fs_registry[] = {
    &fs_ops_ext4,
    &fs_ops_fat
};

const size_t fs_registry_n = sizeof fs_registry / sizeof fs_registry[0];
