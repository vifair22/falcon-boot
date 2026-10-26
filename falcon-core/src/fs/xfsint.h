/* falcon-boot: what xfs.c and xfslog.c share. Not for callers: they have
 * xfs.h.
 */

#ifndef FALCON_FS_XFSINT_H
#define FALCON_FS_XFSINT_H

#include <fs/xfs.h>

/* len bytes at byte `at` of the volume, through the window. */
int xfs_rd(xfs_vol *v, uint64_t at, uint32_t len, void *out);

/* The log's head, and the basic block of the last whole record before it
 * (~0 when there is none). */
int xfs_log_last(xfs_vol *v, uint64_t *head, uint64_t *rec);

/* Replays a log that is not clean into an overlay every read of v then goes
 * through (xfslog.c). XFS_ERR_NOMEM with *need the bytes mem was asked
 * for. */
int xfslog_replay(xfs_vol *v, const xfs_mem *mem, size_t *need);

/* Drops the overlay and gives its memory back. */
void xfslog_release(xfs_vol *v);

/* Nonzero when the overlay holds any of [at, at + len). */
int xfslog_covers(const xfs_vol *v, uint64_t at, uint32_t len);

#endif /* FALCON_FS_XFSINT_H */
