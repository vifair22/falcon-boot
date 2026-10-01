/* falcon-bios: finding the `boot` partition (docs/DESIGN.md Section 3.3).
 *
 * The FAT partition whose boot sector carries an identity record with role
 * `boot` is where stage 3 and the next-boot marker are, at the sectors the
 * record names. Stage 2 finds it on the disk it booted from, and nothing
 * else says where stage 3 is: there is no stage 3 LBA of its own any more.
 * Jump asks the same question of a USB disk to know it is a falcon stick.
 *
 * Host C with no BIOS in it, so the choice is tested (falcon-bios/tests/
 * bootpart_test.c) against images the test builds: stage 2 links it, and a
 * wrong choice there is a machine that does not boot and cannot say why.
 * No .bss and no statics: every buffer is the caller's, as stage 2 has no
 * .bss to give it.
 */

#ifndef FALCON_BIOS_BOOTPART_H
#define FALCON_BIOS_BOOTPART_H

#include <stddef.h>
#include <stdint.h>

#include <part/table.h>
#include <ident/ident.h>

typedef struct {
    uint64_t  start_lba;    /* its first block; 0 for a disk with no table,
                               which is one volume from LBA 0 (a floppy) */
    uint32_t  number;       /* its partition number; 0 with no table */
    ident_rec rec;          /* its record; role IDENT_ROLE_BOOT */
} bootpart;

enum {
    BOOTPART_OK        =  0,
    BOOTPART_ERR_IO    = -1,   /* a read failed */
    BOOTPART_ERR_NONE  = -2,   /* no partition carries a boot record */
    BOOTPART_ERR_TABLE = -3,   /* a partition table, but not a sane one */
    BOOTPART_ERR_ARG   = -4
};

/* Whether a partition's type is one a boot partition can have, which is
 * all the type is used for: it narrows which boot sectors are read. GPT:
 * the ESP type or the falcon boot type. MBR: the FAT type bytes (0x01,
 * 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0xEF) and their hidden forms (0x11, 0x14,
 * 0x16, 0x1B, 0x1C, 0x1E); never an extended container. */
int bootpart_candidate(const part_entry *p);

/* Reads the disk's table and returns, in *out, the first candidate whose
 * boot sector decodes as a `boot` record naming a stage 3 (its stage 3
 * sector not 0), in table order: a station installed for UEFI and BIOS both
 * has a boot record with no stage 3 on its ESP, which comes first on a GPT
 * disk, and the real one on its station partition. A disk with no
 * table is one volume: its LBA 0 is asked instead. On success `sec` holds
 * that boot sector (block_size bytes; the caller's, at least block_size
 * long), for bootpart_stage3_room.
 *
 * `parts`/`cap` is the partition list's storage; disk_blocks is the disk's
 * length in blocks, or 0 when unknown, as part_read_table takes them. A
 * table with more partitions than fit is searched as far as it fitted. */
int bootpart_find(gpt_read_fn read, void *ctx, uint32_t block_size,
                  uint64_t disk_blocks, part_entry *parts, uint32_t cap,
                  uint8_t *sec, bootpart *out);

/* How many sectors stage 3 may occupy: from the record's stage 3 sector to
 * the end of the volume's reserved sectors (the FAT BPB's count at 0x0E),
 * since stage 3 lives inside them and anything past them is the FAT. 0 when
 * the record's sector is not inside the reserved area at all. */
uint32_t bootpart_stage3_room(const uint8_t *bootsec, const ident_rec *rec);

#endif /* FALCON_BIOS_BOOTPART_H */
