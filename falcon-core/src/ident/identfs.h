/* falcon-core: a volume's identity record, read through the filesystem
 * registry (docs/DESIGN.md Section 3.1).
 *
 * The ISO library (exFAT) and a plaintext shelf (ext4) carry their record as
 * a file, /.falcon, one line of ident_encode_text. This is the one reader of
 * that file, for every consumer that finds a partition by record: both
 * loaders' scans, falcon-hook, falcon-winjump and falcon-vdisk (through
 * media/extfind.h), the appliance and the host tools. ident.c stays free of
 * the filesystem code on purpose (stage 2 links it alone), so the reading
 * half is here.
 *
 * EFI-free and allocation-free: the caller owns the scratch, which is
 * several kilobytes (a directory iterator, a file handle, a dirent and, for
 * ident_probe, the volume) and so is never a local in any build that polices
 * its stack.
 */

#ifndef FALCON_IDENTFS_H
#define FALCON_IDENTFS_H

#include <fs/fs.h>
#include <ident/ident.h>

typedef struct {
    fs_vol    vol;      /* ident_probe's mount; unused by ident_read_vol */
    fs_dir    dir;
    fs_file   file;
    fs_dirent ent;
} ident_fs_scratch;

/* Reads /.falcon off a mounted volume. IDENT_OK with *out filled;
 * IDENT_ERR_NONE when the volume has no such file, or has a directory or a
 * device node by that name; IDENT_ERR_BAD when the file is there but is not
 * exactly one record (longer than IDENT_TEXT_MAX, unreadable, or refused by
 * ident_decode_text); IDENT_ERR_ARG. Every answer but IDENT_OK means "this
 * volume is not identified", and no caller does more with the difference
 * than say it on a diagnostic line. */
int ident_read_vol(fs_vol *v, ident_fs_scratch *s, ident_rec *out);

/* The same for a volume not yet mounted: the registry identifies whatever
 * filesystem is on read/ctx, mounts it into s->vol and reads its /.falcon.
 * IDENT_ERR_NONE as well when no registered driver takes the volume. s->vol
 * stays mounted afterwards, so a caller that goes on to use the volume need
 * not mount it again. */
int ident_probe(fs_read_fn read, void *ctx, ident_fs_scratch *s,
                ident_rec *out);

#endif /* FALCON_IDENTFS_H */
