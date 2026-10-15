/* falcon-bios: what the C half of the loader may ask of the assembly half.
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

#include <start/biosdefs.h>
#include <vfd/vfdcore.h>

/* ---- the thunk (falcon-bios/src/start/thunk.S) ----
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
    uint16_t es;      /* in and out, as bp */
    uint16_t bp;
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

/* ---- absolute low memory ----
 *
 * The interrupt vector table and the BIOS data area are at addresses the
 * firmware fixed decades ago, and a loader reads and writes them directly.
 * Through inline asm rather than a cast pointer because a compiler is
 * entitled to assume a dereference near zero is a null-pointer bug and warns
 * that it is one; saying it in asm says instead that the address is the
 * point. */
static inline uint8_t peek8(uint32_t addr)
{
    uint8_t v;

    __asm__ __volatile__("movb (%1), %0"
                         : "=q"(v) : "r"((uintptr_t)addr) : "memory");
    return v;
}

static inline uint16_t peek16(uint32_t addr)
{
    uint16_t v;

    __asm__ __volatile__("movw (%1), %0"
                         : "=r"(v) : "r"((uintptr_t)addr) : "memory");
    return v;
}

static inline void poke8(uint32_t addr, uint8_t v)
{
    __asm__ __volatile__("movb %0, (%1)"
                         : : "q"(v), "r"((uintptr_t)addr) : "memory");
}

static inline void poke16(uint32_t addr, uint16_t v)
{
    __asm__ __volatile__("movw %0, (%1)"
                         : : "r"(v), "r"((uintptr_t)addr) : "memory");
}

static inline uint32_t peek32(uint32_t addr)
{
    uint32_t v;

    __asm__ __volatile__("movl (%1), %0"
                         : "=r"(v) : "r"((uintptr_t)addr) : "memory");
    return v;
}

static inline void poke32(uint32_t addr, uint32_t v)
{
    __asm__ __volatile__("movl %0, (%1)"
                         : : "r"(v), "r"((uintptr_t)addr) : "memory");
}

/* ---- console ---- */

/* Serial. Always on: it is what the rig reads and what a machine with no
 * usable video still has. */
void bios_putc(char c);
void bios_puts(const char *s);

/* Teletype output to the screen and serial together, for bios_fatal: the one
 * screen stage 2 can draw, since it has no layout to draw with. Stage 3
 * draws everything else through screen.h. */
void bios_screen_puts(const char *s);

/* One character to the screen alone, through teletype: stage 2's loading
 * line and its spinner, which serial does not want. */
void bios_screen_putc(char c);

/* One character of a UCS-2 name as this build can print it.
 *
 * Written with an if rather than a conditional expression because the two
 * arms of one are promoted to int and the result then narrows on the way
 * back into a char, which is implementation-defined and which clang-tidy is
 * right to say so about. */
static inline char bios_ascii(uint16_t c, char fallback)
{
    if (c < 0x80u)
        return (char)c;
    return fallback;
}

/* Blocks. Returns one of BIOS_KEY_* for the keys a menu needs, or the ASCII
 * character otherwise. */
int bios_getkey(void);

/* Whether a key is waiting in the BIOS's ring (INT 16h AH=01h), without
 * taking it and without polling. Nonzero if so. */
int bios_key_ready(void);

/* Polls the keyboards the BIOS cannot read and stage 3 can: falcon jump's
 * USB modules, kept up after a route came back (DESIGN Section 13.6). Each
 * key goes into the BIOS's ring, where bios_key_ready and INT 16h find it.
 * Nonzero when there are such keyboards; a stage without them links a
 * no-op that says 0. */
int bios_key_poll(void);

/* Zeroes the BIOS's keyboard ring buffer. INT 16h hands a key over by
 * moving the ring's head past it, not by erasing it, so after a passphrase
 * the last fifteen or so of its characters are still in the BIOS data area
 * for anything that reads low memory. */
void bios_key_buffer_wipe(void);

/* The same numbers as falcon-core's TUI_KEY_*, so a key goes to the
 * screens as it is read. */
#define BIOS_KEY_UP     0x100
#define BIOS_KEY_DOWN   0x101
#define BIOS_KEY_PGUP   0x102
#define BIOS_KEY_PGDN   0x103
#define BIOS_KEY_HOME   0x104
#define BIOS_KEY_END    0x105
#define BIOS_KEY_ENTER  0x00D
#define BIOS_KEY_ESC    0x01B
/* A page's wait for a key: falcon-core's tui_wait where stage 3 has its
 * screens up (and so the idle power-off, console/display.c), bios_getkey
 * where it does not. */
int bios_wait_key(void);

/* Resets the machine. Does not return. */
__attribute__((noreturn)) void bios_reboot(void);

/* The end of every failure the loader cannot go on from: `why` on the
 * screen and on serial, then a key, then a reset. Never a halt, which on a
 * machine with no serial cable looks exactly like a hang. Does not return. */
__attribute__((noreturn)) void bios_fatal(const char *why);
void bios_put_hex64(uint64_t v);
void bios_put_dec(uint64_t v);

/* The disk address packet INT 13h AH=42h and AH=43h read, at the fixed low
 * address biosdefs.h names rather than as a static.
 *
 * A static was reachable while disk.c only ever ran in stage 2, which is
 * below a megabyte; the same file links into stage 3, which is not, and real
 * mode truncated the address to sixteen bits and handed the BIOS whatever
 * was there. Every read returned carry. */
typedef struct {
    uint8_t  size;
    uint8_t  reserved;
    uint16_t sectors;
    uint16_t offset;
    uint16_t segment;
    uint64_t lba;
} bios_dap;

#define bios_dap_at (*(bios_dap *)(uintptr_t)DAP_ADDR)

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
    /* Zero sectors: read by LBA, AH=42h. Otherwise the drive has no INT 13h
     * extensions (a floppy, DESIGN Section 13.3) and is read by AH=02h with
     * the geometry AH=08h gave. */
    vfd_geom geom;
} bios_disk;

int bios_disk_read(void *ctx, uint64_t off, uint64_t len, void *buf);


/* bios_disk_read for any length at any offset (disk/bytes.c, DESIGN Section
 * 3.4): the callback a driver is handed, since a driver reads 512 or 1024
 * bytes where a 4Kn disk's sector is 4096. Stage 3 only; `ctx` as above. A
 * request of whole sectors is passed to bios_disk_read unchanged. */
int bios_disk_bytes(void *ctx, uint64_t off, uint64_t len, void *buf);

/* A bios_disk_bytes read longer than this is made in pieces of it, with the
 * hook below called before each. */
#define BIOS_DISK_PIECE 0x40000u   /* 256 KiB */

/* Called before every bios_disk_bytes read and every piece of a long one;
 * NULL (the default) for none. The stick's background probe yields to the
 * menu from it (task/task.h). Between pieces the re-blocking holds nothing,
 * so the main flow may read while the probe is suspended there. Stage 3's
 * alone: stage 2 has no .bss to keep it in. */
void bios_disk_set_hook(void (*hook)(void));

/* Called after every INT 13h read call that succeeds (a CHS track, an AH=42h
 * bounce buffer); NULL (the default) for none. Stage 2's spinner turns from
 * it while stage 3 is read in (start/load3.c), and stage 3's display turns
 * a step's spinner from it (tui_tick). It lives in .data, so stage 2 can
 * keep it, and each stage's copy of disk.c has its own. */
void bios_disk_on_call(void (*fn)(void));

/* Empties the track cache, so the next read of any sector goes to the disk.
 * For a read whose checksum failed: the retry has to read the medium again,
 * not the copy the first attempt left behind. */
void bios_disk_forget(void);

/* Fills in `sector_size` by asking the BIOS about the drive, and says
 * whether the drive answered at all. Nonzero on success. A drive that
 * answers neither AH=48h nor AH=41h but reports a geometry through AH=08h is
 * read by CHS from then on. */
int bios_disk_probe(bios_disk *d, uint8_t drive);

/* Entered from stage 2 once protected mode is up. Never returns. */
void bios_main(uint8_t drive);

#endif /* FALCON_BIOS_H */
