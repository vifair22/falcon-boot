/* falcon-boot: crc32c (Castagnoli), the checksum of ext4's metadata and of
 * its journal (jbd2), which both drivers share.
 */

#ifndef FALCON_FS_CRC32C_H
#define FALCON_FS_CRC32C_H

#include <stdint.h>
#include <stddef.h>

/* Running update, matching the kernel's ext4_chksum() and jbd2_chksum(): no
 * initial or final inversion, so callers chain pieces of a structure
 * together and seed the first piece themselves (~0 for a superblock). */
uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n);

/* n zero bytes, for the checksum field a structure is hashed without. */
uint32_t crc32c_zeros(uint32_t crc, size_t n);

#endif /* FALCON_FS_CRC32C_H */
