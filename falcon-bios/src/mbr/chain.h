/* falcon-bios: the station's two chains, a partition's boot sector and
 * another disk's MBR, with the drive swap (docs/DESIGN.md Section 13.4).
 * See chain.c; the decisions are vbrcore.c's.
 */

#ifndef FALCON_BIOS_CHAIN_H
#define FALCON_BIOS_CHAIN_H

#include <start/bios.h>
#include <result.h>
#include <part/table.h>
#include <mbr/vbrcore.h>

/* What a primary partition's boot sector is, for the menu: its loader by
 * signature, or VBR_LOADER_NONE when it carries none, will not read, or is
 * not a partition this can chain (a logical one, an extended container, one
 * on a GPT disk or on a disk whose sectors are not 512 bytes). */
vbr_loader chain_part_loader(bios_disk *d, const part_entry *p);

/* Whether the disk's LBA 0 is one to offer: vbr_mbr_bootable. */
int chain_disk_bootable(bios_disk *d);

/* The drive number the booted system is handed in DL: 0x80 when `swap` is
 * set and the disk is not 0x80 already (the swap resident then makes it
 * so), else the disk's own. */
uint8_t chain_dl(const bios_disk *d, int swap);

/* Starts partition `p` of disk `d` as a DOS or Windows MBR would: LBA 0 to
 * 0000:0600 with p's entry marked active in that copy, DS:SI at the entry,
 * the partition's first sector to 0000:7C00 with its BPB made to agree
 * (vbr_fix_bpb, in memory only), DL = chain_dl, and with `swap` the drive
 * swap installed. Returns only when it did not get that far, with *r
 * filled in and nothing installed. */
int chain_part(bios_disk *d, const part_entry *p, int swap, boot_result *r);

/* Starts disk `d`'s own LBA 0 at 0000:7C00 with DL = chain_dl, the swap
 * installed with `swap`. Returns as chain_part. */
int chain_disk(bios_disk *d, int swap, boot_result *r);

#endif /* FALCON_BIOS_CHAIN_H */
