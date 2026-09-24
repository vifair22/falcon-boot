/* falcon-core: what the filesystem adapters (fs_<driver>.c) and the registry
 * tables (fs_reg_*.c) share with fs.c. Internal to fs/; no consumer includes
 * it. */

#ifndef FALCON_FS_ADAPTER_H
#define FALCON_FS_ADAPTER_H

#include <stddef.h>

#include <fs/fs.h>

/* One row per driver status, driver value to unified value. The drivers'
 * own enums are never renumbered: their tests pin those values. */
typedef struct {
    int driver;
    int unified;
} fs_status_row;

/* The unified status for a driver's rc. Non-negative values pass through
 * (OK, and dir_next's "1 = entry produced"); a status with no row maps to
 * FS_ERR_CORRUPT rather than leak an unmapped number. */
int fs_map_status(const fs_status_row *rows, size_t n, int rc);

#define FS_STATUS_MAP(rows, rc) \
    fs_map_status((rows), sizeof(rows) / sizeof((rows)[0]), (rc))

/* The table fs_identify, fs_by_name and fs_driver_at walk, in probe order.
 * Exactly one of fs_reg_all.c and fs_reg_ext4.c defines it. */
extern const fs_ops *const fs_registry[];
extern const size_t fs_registry_n;

#endif /* FALCON_FS_ADAPTER_H */
