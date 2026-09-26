/* falcon-bios: the arithmetic of the x86 boot protocol, with no memory
 * writes in it (linux.c does those).
 *
 * Here because every number in it is a chance to hand the kernel an address
 * it cannot use, and the failures do not say so: a kernel loaded past the
 * end of what the loader can address triple-faults, and an initrd segment
 * that starts
 * one byte short of a 4-byte boundary produces
 *
 *   Initramfs unpacking failed: invalid magic at start of compressed archive
 *
 * and a system with no root. Both have happened here. The half that decides
 * WHERE is this file, and it is ordinary C a test can hand any header to.
 */

#ifndef FALCON_BIOS_LINUXCORE_H
#define FALCON_BIOS_LINUXCORE_H

#include <stddef.h>
#include <stdint.h>

/* Setup header fields, at their offsets in the first two sectors of a
 * bzImage. Two sectors and not one: setup_sects is at 0x1F1, inside the boot
 * sector, but the "HdrS" magic that says the header is a header is at 0x202,
 * past its end. */
#define HDR_SETUP_SECTS      0x1F1  /* u8   512-byte sectors of setup code */
#define HDR_SYSSIZE          0x1F4  /* u32  16-byte paras of the pm kernel */
#define HDR_BOOT_FLAG        0x1FE  /* u16  0xAA55 */
#define HDR_HEADER           0x202  /* u32  "HdrS" */
#define HDR_VERSION          0x206  /* u16  boot protocol version */
#define HDR_TYPE_OF_LOADER   0x210  /* u8 */
#define HDR_LOADFLAGS        0x211  /* u8 */
#define HDR_CODE32_START     0x214  /* u32  where the pm kernel was loaded */
#define HDR_RAMDISK_IMAGE    0x218  /* u32 */
#define HDR_RAMDISK_SIZE     0x21C  /* u32 */
#define HDR_HEAP_END_PTR     0x224  /* u16 */
#define HDR_CMD_LINE_PTR     0x228  /* u32 */
#define HDR_INITRD_ADDR_MAX  0x22C  /* u32 */
#define HDR_KERNEL_ALIGNMENT 0x230  /* u32 */
#define HDR_RELOCATABLE      0x234  /* u8 */
#define HDR_MIN_ALIGNMENT    0x235  /* u8   log2 */
#define HDR_XLOADFLAGS       0x236  /* u16 */
#define HDR_CMDLINE_SIZE     0x238  /* u32 */
#define HDR_PREF_ADDRESS     0x258  /* u64 */
#define HDR_INIT_SIZE        0x260  /* u32  memory the kernel needs to run */
#define HDR_JUMP_LEN         0x201  /* u8 */

#define HDRS_MAGIC     0x53726448u  /* "HdrS" */
#define XLF_KERNEL_64  0x0001u      /* a 64-bit kernel */

/* The protocol version below which this loader declines rather than guesses.
 * 2.12 is where xloadflags and init_size are both present and settled;
 * anything on a distribution ISO is well past it. */
#define LINUXCORE_MIN_VERSION 0x020Cu

typedef enum {
    LINUXCORE_OK              =  0,
    LINUXCORE_ERR_ARG         = -1,
    LINUXCORE_ERR_TOO_SMALL   = -2,  /* not even a setup header long */
    LINUXCORE_ERR_NOT_BZIMAGE = -3,  /* no 0xAA55, or no "HdrS" */
    LINUXCORE_ERR_VERSION     = -4,  /* older than LINUXCORE_MIN_VERSION */
    LINUXCORE_ERR_SETUP_LONG  = -6,  /* setup longer than the cap given */
    LINUXCORE_ERR_NO_KERNEL   = -7,  /* all setup and no kernel */
    LINUXCORE_ERR_NOT_RELOC   = -8,  /* fixed, and names no address */
    LINUXCORE_ERR_NO_ROOM     = -9,  /* wants more than the loader reaches */
    LINUXCORE_ERR_TOO_LOW     = -10  /* fixed, below where a kernel may go */
} linuxcore_status;

typedef struct {
    uint64_t setup_len;    /* bytes of setup, from the file's start */
    uint64_t pm_len;       /* the protected-mode half that follows it */
    uint64_t load_at;      /* where the protected-mode half goes */
    uint64_t init_size;    /* what the kernel needs to run in, >= pm_len */
    uint64_t initrd_max;   /* highest byte an initrd may occupy */
    uint32_t cmdline_cap;  /* including the terminator */
    uint16_t version;
    uint8_t  is_64;        /* a 64-bit kernel (xloadflags) */
} linux_geom;

/* Reads the geometry out of `setup` (at least 1024 bytes of the kernel's
 * start) for a kernel of `file_len` bytes, refusing every shape this loader
 * cannot boot. `pref_load` is where a relocatable kernel is placed before
 * alignment and the lowest address any kernel may take, since everything
 * below it is the loader's own; `map_end` one past the last byte the loader
 * can address, and
 * `setup_cap` the buffer the caller will read the setup into. */
linuxcore_status linuxcore_geometry(const uint8_t *setup, size_t setup_len,
                                    uint64_t file_len, uint64_t pref_load,
                                    uint64_t map_end, uint64_t setup_cap,
                                    linux_geom *out);

/* Where one initrd lands in a concatenation, and how much zero follows it.
 *
 * EVERY SEGMENT IS PADDED TO FOUR BYTES. The kernel's unpacker walks a
 * concatenation of archives and expects each to begin aligned; one byte late
 * is read as a corrupt header. Debian's initrd is 113,383,147 bytes, so
 * appending falcon-hook straight onto it produced exactly that. Arch's
 * happens to be a multiple of four, which is why that medium got further and
 * looked like a different bug. */
uint64_t linuxcore_align4(uint64_t v);

/* Total bytes a concatenation of `n` initrds of `sizes` occupies, padding
 * included. */
uint64_t linuxcore_initrd_len(const uint64_t *sizes, unsigned n);

/* The offset of initrd `i` within that concatenation, and the pad after it.
 * `pad_out` may be NULL. */
uint64_t linuxcore_initrd_at(const uint64_t *sizes, unsigned n, unsigned i,
                             uint64_t *pad_out);

#endif /* FALCON_BIOS_LINUXCORE_H */
