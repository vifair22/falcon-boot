/* The full filesystem registry: every driver, for the stick's loaders, the
 * host tools and the appliance.
 *
 * Probe order for a volume with no role hint. exFAT first because p2 is the
 * partition a stick has most of, and because its magic sits in the first
 * sector where ext4's is a kilobyte in.
 *
 * The two media drivers come last, and UDF before ISO9660, because a bridge
 * disc answers both and the UDF side is the complete one: Windows media's
 * ISO9660 tree is a README and nothing else. A plain ISO9660 image has no
 * NSR descriptor, so udf_probe passes it on.
 */

#include <fs/fs_adapter.h>

const fs_ops *const fs_registry[] = {
    &fs_ops_exfat,
    &fs_ops_ext4,
    &fs_ops_udf,
    &fs_ops_iso9660,
    &fs_ops_fat,
    &fs_ops_btrfs,
    &fs_ops_xfs
};

const size_t fs_registry_n = sizeof fs_registry / sizeof fs_registry[0];
