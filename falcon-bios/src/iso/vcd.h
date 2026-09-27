/* falcon-bios: serving an ISO as a CD to its own bootloader. See vcd.c. */

#ifndef FALCON_BIOS_VCD_H
#define FALCON_BIOS_VCD_H

#include <start/bios.h>
#include <result.h>
#include <fs/fs.h>
#include <part/table.h>
#include <media/eltorito.h>

/* The drive number the served medium answers on.
 *
 * 0xE0 is the first of the range El Torito reserves for CDs and is what a
 * BIOS that boots one from an ISO hands over in DL, which is what the loaded
 * program reads to find its own medium. */
#define VCD_DRIVE 0xE0u

/* Installs the resident handler over the ISO named by `iso` on the mounted
 * p2 volume, loads the medium's own boot image where El Torito says, and
 * hands the machine to it. Does not return on success.
 *
 * Returns nonzero with *r filled in when the medium cannot be served: no
 * BIOS boot image in its catalog, or an ISO in more pieces on p2 than the
 * handler will scan. What it hands over is logged to serial. */
int vcd_chainload(bios_disk *d, fs_vol *p2, const part_entry *p2part,
                  const fs_dirent *iso, fs_file *isofile, boot_result *r);

/* The same for a whole drive whose sectors are the medium's CD sectors
 * (falcon jump's CD route, DESIGN Section 14.6: a drive a module answers
 * through the reverse thunk). `n_cd` is the disc's length in CD sectors. */
int vcd_chainload_drive(bios_disk *d, uint64_t n_cd, boot_result *r);

/* Memory the caller keeps in use behind the handoff, carved out of the map
 * the medium is handed along with the virtual CD's own. At most two. */
int vcd_keep(uint64_t base, uint64_t len);
void vcd_keep_reset(void);

/* The catalog entry a BIOS boots: the default entry, or the first that says
 * x86 (media/eltorito.c's header says why the platform is not trusted
 * first). NULL when the medium boots only on UEFI. */
const elt_entry *vcd_boot_entry(const elt_catalog *cat);

/* Serves `len` bytes at `iso_off` from memory rather than from the disk when
 * the medium is next chainloaded. This is iso/overlay.h's job under UEFI and
 * is what makes a medium bootable that is not bootable as it lies on the
 * stick; media/bsdmd.c's two edits are what need it first.
 *
 * Whole CD sectors are what the handler can serve, so the sectors holding
 * the edit are read from the ISO, the edit is laid over them, and the result
 * is what is served. Call before vcd_chainload; vcd_overlay_reset forgets
 * everything staged so far.
 *
 * 0 on success, nonzero when the pool is full or the sectors would not
 * read. */
int vcd_overlay(fs_file *isofile, uint64_t iso_off, const void *bytes,
                uint32_t len);
void vcd_overlay_reset(void);

/* Serves `n_cd` CD sectors past the ISO's own end from `phys`, which is a
 * physical address above a megabyte: the medium grows by that much and the
 * handler has the BIOS move the bytes with INT 15h AH=87h.
 *
 * Windows needs it and nothing else does: its patched boot.wim is longer
 * than the original, and the difference is tens of megabytes, which is more
 * conventional memory than a machine has to give. */
int vcd_overlay_append(uint32_t phys, uint32_t n_cd);

/* Serves one CD sector at `cd_lba` from the given 2048 bytes, which are copied
 * into the handler's pool. Unlike vcd_overlay it does not read a base sector
 * off the ISO first, so `cd_lba` may be one the ISO does not have: the
 * relocated Windows boot.wim's patched header sits at the appended run's start,
 * over the original header the redirect otherwise serves from disk. Call before
 * vcd_chainload. */
int vcd_overlay_at(uint32_t cd_lba, const uint8_t *sector);

/* Redirects `n_cd` CD sectors of the medium grown past the ISO's end back to
 * where CD sector `src_cd_lba` and the ones after it already lie on the disk,
 * so a relocated file's bulk is served over INT 13h. The redirected region is
 * placed at the current end of the medium, before any vcd_overlay_append tail;
 * its first sector is where a relocated file's record must point. Call before
 * vcd_chainload; one redirect per medium. */
int vcd_overlay_redirect(uint32_t src_cd_lba, uint32_t n_cd);

#endif /* FALCON_BIOS_VCD_H */
