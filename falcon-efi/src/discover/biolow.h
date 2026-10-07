/* falcon-boot: Block I/O reads through memory below 4 GiB (docs/DESIGN.md
 * Section 3.1).
 *
 * A firmware that starts the loader itself, rather than through shim, may
 * place the image above 4 GiB, and with it every static buffer; an ASUS
 * B550's AMI NVMe driver then refuses the read with EFI_UNSUPPORTED rather
 * than bouncing it. A read whose buffer reaches above 4 GiB therefore goes
 * through a buffer below it, a chunk at a time.
 *
 * EFI-free apart from the protocol it is handed, so the host suite drives it
 * with a fake Block I/O that refuses as that driver does.
 */

#ifndef FALCON_BIOLOW_H
#define FALCON_BIOLOW_H

#include <efi/types.h>
#include <efi/blockio.h>
#include <efi/status.h>

/* The first address a buffer may not reach. */
#define BIOLOW_LIMIT 0x100000000ull

/* Returns the memory below BIOLOW_LIMIT to read through, or NULL when there
 * is none; called only for a read that needs it, so the caller can allocate
 * on first need. */
typedef UINT8 *(*biolow_fn)(void);

/* ReadBlocks len bytes (whole blocks) at lba into buf. A buf that ends at or
 * below BIOLOW_LIMIT is read directly. Otherwise the read goes through
 * low(), low_len bytes of it at a time rounded down to whole blocks; with no
 * such memory, or less of it than one block, it is read directly and the
 * firmware's answer stands. The first failing ReadBlocks's status is
 * returned. */
EFI_STATUS biolow_read(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 lba, UINTN len,
                       void *buf, biolow_fn low, UINTN low_len);

#endif
