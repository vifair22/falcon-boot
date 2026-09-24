/* falcon-bios: booting a Linux kernel directly (docs/notes/bios-route.md
 * Section 4.2). See linux.c.
 */

#ifndef FALCON_BIOS_LINUX_H
#define FALCON_BIOS_LINUX_H

#include <stddef.h>
#include <stdint.h>

/* Reads a whole object into memory. `dst` is a physical address; the callback
 * is whatever the caller has open, and is read from `off` for `len` bytes.
 * 0 on success. This is how linux_boot stays ignorant of where a kernel came
 * from: an ISO9660 file, a file on p2, or anything else with a read. */
typedef int (*linux_read_fn)(void *ctx, uint64_t off, uint64_t len, void *buf);

typedef struct {
    linux_read_fn read;
    void         *ctx;
    uint64_t      size;
} linux_object;

/* Loads kernel and initrds, fills the zero page, and jumps. Does not return
 * on success. Returns nonzero when the kernel is one this loader will not
 * boot, or when something would not fit, with the reason in linux_why() and
 * on serial.
 *
 * `initrd` may be several objects; they are concatenated in order, which is
 * what the kernel expects of a multi-initrd boot and what lets falcon-hook be
 * appended to whatever the ISO already carries.
 *
 * `last`, when not NULL, runs after every object has been read and copied
 * into place and before the jump, which is the one moment a caller holding
 * secrets can let go of them: the reads are done, and nothing of this loader
 * runs again. */
int linux_boot(const linux_object *kernel, const linux_object *initrd,
               unsigned n_initrd, const char *cmdline, void (*last)(void));

/* The reason the last linux_boot returned, as a sentence without its full
 * stop; NULL before any failure. */
const char *linux_why(void);

/* Whether the processor has long mode, which a 64-bit kernel needs and a
 * 32-bit one does not. */
int linux_cpu_is_64(void);

#endif /* FALCON_BIOS_LINUX_H */
