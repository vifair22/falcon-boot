/* falcon-bios: what the C half of the loader may ask of the assembly half
 * (docs/notes/bios-route.md Section 5.3).
 *
 * The rule that shapes this file: the assembly is transport and nothing
 * else. Five jobs are allowed to be written in it (the MBR, the mode switch,
 * the thunk, the INT 13h resident's prologue, and the jump into a loaded
 * kernel), and everything decidable is C, here, where the repository's
 * analyzers and the host test suite can reach it.
 *
 * So this header is deliberately small. It is a register block, a way to
 * raise an interrupt, and a byte of serial output; everything a reader might
 * expect to find here - partition tables, filesystems, menus - is in
 * falcon-core already and does not know which firmware it is running on.
 */

#ifndef FALCON_BIOS_H
#define FALCON_BIOS_H

#include <stddef.h>
#include <stdint.h>

#include "biosdefs.h"

/* ---- the thunk (falcon-bios/src/thunk.S) ----
 *
 * The register block IS the interface, because nothing else survives the
 * round trip down to real mode and back: no stack, no pointer, nothing that
 * assumes an address above a megabyte. Fill it, set the vector, call, and
 * read the results out of the same place.
 *
 * It lives in low memory so real mode can reach it, which is why it is a
 * definition in the assembly rather than anything C allocates. */
typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esi;
    uint32_t edi;
    uint16_t es;
    uint16_t pad0;
    uint16_t flags;   /* the carry bit is how a BIOS call says it failed */
    uint16_t pad1;
} bios_regs;

/* Reached at the fixed addresses biosdefs.h names rather than by symbol,
 * because stage 3 is linked separately and above a megabyte, and the thunk
 * cannot be linked into it: real mode has twenty address bits. Both stages
 * therefore say the same thing here and neither needs to know which it is. */
#define thunk_regs   (*(volatile bios_regs *)(uintptr_t)GATE_REGS)
#define thunk_vector (*(volatile uint8_t *)(uintptr_t)GATE_VECTOR)
#define bios_boot_drive (*(volatile uint8_t *)(uintptr_t)GATE_DRIVE)

static inline void thunk_bios(void)
{
    ((void (*)(void))(uintptr_t)GATE_THUNK)();
}

#define BIOS_FLAG_CARRY 0x0001u

/* ---- console ---- */

/* Serial. Always on: it is what the rig reads and what a machine with no
 * usable video still has. */
void bios_putc(char c);
void bios_puts(const char *s);

/* The screen and the keyboard, through the BIOS. Both are one thunk call
 * per character, which is slow and is the right trade for a menu: the
 * alternative is a VGA driver and a PS/2 driver of our own, and neither
 * would work on a machine whose firmware is doing something unusual.
 *
 * bios_screen_puts writes to both the screen and serial, so a caller does
 * not choose and a log and a display never disagree. */
void bios_screen_puts(const char *s);
void bios_screen_clear(void);
void bios_screen_at(uint8_t row, uint8_t col);

/* Blocks. Returns one of BIOS_KEY_* for the keys a menu needs, or the ASCII
 * character otherwise. */
int bios_getkey(void);

#define BIOS_KEY_UP     0x100
#define BIOS_KEY_DOWN   0x101
#define BIOS_KEY_ENTER  0x00D
#define BIOS_KEY_ESC    0x01B
void bios_put_hex64(uint64_t v);
void bios_put_dec(uint64_t v);

/* ---- disk ----
 *
 * The whole of the per-platform boundary, and the reason falcon-core needs no
 * changes at all to run here: this has the same shape as every read callback
 * in the tree, and what is on the other end of it is the BIOS rather than a
 * firmware Block I/O protocol or a host file.
 *
 * `ctx` is a bios_disk. Returns 0 on success, nonzero on failure, exactly as
 * fs_read_fn and gpt_read_fn require. */
typedef struct {
    uint8_t  drive;         /* what the BIOS booted from; rarely 0x80 */
    uint32_t sector_size;   /* 512 for a disk, 2048 for a CD */
} bios_disk;

int bios_disk_read(void *ctx, uint64_t off, uint64_t len, void *buf);

/* Fills in `sector_size` by asking the BIOS about the drive, and says
 * whether the drive answered at all. Nonzero on success. */
int bios_disk_probe(bios_disk *d, uint8_t drive);

/* Entered from stage 2 once long mode is up. Never returns. */
void bios_main(uint8_t drive);

#endif /* FALCON_BIOS_H */
