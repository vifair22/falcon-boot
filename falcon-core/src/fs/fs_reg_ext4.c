/* The stationary build's filesystem registry (DESIGN Section 12): ext4, the
 * shelf's filesystem, and nothing else. Linking this table instead of
 * fs_reg_all.c is what keeps the other drivers out of that build, so their
 * probes never run over the machine's disks.
 */

#include <fs/fs_adapter.h>

const fs_ops *const fs_registry[] = {
    &fs_ops_ext4
};

const size_t fs_registry_n = sizeof fs_registry / sizeof fs_registry[0];
