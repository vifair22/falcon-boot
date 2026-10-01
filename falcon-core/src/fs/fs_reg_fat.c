/* falcon jump's filesystem registry (DESIGN Section 13.3): FAT, the floppy's
 * filesystem, and nothing else. As with fs_reg_ext4.c, linking this table
 * instead of fs_reg_all.c is what keeps the other drivers out of that build.
 */

#include <fs/fs_adapter.h>

const fs_ops *const fs_registry[] = {
    &fs_ops_fat
};

const size_t fs_registry_n = sizeof fs_registry / sizeof fs_registry[0];
