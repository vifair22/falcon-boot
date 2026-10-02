/* falcon jump's PCC.MOD: a PC Card's card information structure and the
 * ExCA window registers, apart from the socket they describe (DESIGN
 * Section 13.4).
 *
 * A 16-bit PC Card says what it is in its CIS, a chain of tuples in its
 * attribute memory; a CardBus bridge reaches the card through windows it
 * decodes, set in its ExCA registers (the i82365's register set, which every
 * bridge of the era copies). Both are layouts a byte out of place in which
 * maps the wrong memory or configures the card for an I/O range nothing is
 * listening on, and neither says so. They are here, host-tested; pcc.c is
 * the socket.
 */

#ifndef FALCON_BIOS_PCCCORE_H
#define FALCON_BIOS_PCCCORE_H

#include <stddef.h>
#include <stdint.h>

#define CISTPL_NULL         0x00u
#define CISTPL_FUNCID       0x21u
#define CISTPL_CONFIG       0x1Au
#define CISTPL_CFTABLE      0x1Bu
#define CISTPL_END          0xFFu

#define FUNCID_FIXED_DISK   4u

typedef struct {
    uint8_t  funcid;          /* CISTPL_FUNCID's function, 0xFF when absent */
    uint32_t config_base;     /* attribute memory address of the COR */
    uint8_t  index;           /* the configuration index to write */
    /* The I/O the chosen entry wants: `io_lines` address lines decoded,
     * with no fixed range (any 2^io_lines aligned base), or the fixed range
     * [io_base, io_base + io_len). */
    uint8_t  io_lines;
    uint16_t io_base;
    uint16_t io_len;
} pcc_cis;

/* Walks `n` CIS bytes (one per tuple byte, already taken from the even
 * attribute addresses) and picks the configuration entry to use: the first
 * that asks for I/O and decodes any base (the CompactFlash standard's
 * "contiguous I/O" entry), else the first with a fixed I/O range. 0 when
 * there is a CISTPL_CONFIG and such an entry; -1 otherwise. */
int pcc_cis_parse(const uint8_t *cis, size_t n, pcc_cis *out);

/* The six ExCA bytes (registers 0x10-0x15 for memory window 0) mapping
 * `size` bytes of host memory at `host` (4 KB aligned, below 16 MB) onto
 * card memory at `card`, attribute memory when `attr`. -1 on an address
 * the window cannot express. */
int pcc_mem_window(uint32_t host, uint32_t size, uint32_t card, int attr,
                   uint8_t regs[6]);

/* The four ExCA bytes (registers 0x08-0x0B for I/O window 0) mapping
 * [base, base + len). -1 on a range the window cannot express. */
int pcc_io_window(uint16_t base, uint16_t len, uint8_t regs[4]);

/* An I/O window's ports, as the caller reads them before the window is
 * placed: PCC_IO_PORTS bytes from the base. */
#define PCC_IO_PORTS 16u

/* Whether a range that read as `ports` is free: every port 0xFF, a bus
 * nothing drives. Anything answering at one of them reads otherwise. */
int pcc_io_free(const uint8_t *ports);

/* The option-ROM area the attribute memory window is placed in, as the
 * caller reads it: the first three bytes at every 2 KB boundary from
 * PCC_ROM_BASE, PCC_ROM_SLOTS of them, three bytes each. It stops at
 * 0xE0000: on many machines (Phoenix, Award) 0xE0000-0xEFFFF is the system
 * BIOS, with no header to find, and the BIOS flash may answer there too. */
#define PCC_ROM_BASE   0x000C0000u
#define PCC_ROM_END    0x000E0000u        /* exclusive */
#define PCC_ROM_SLOT   0x800u
#define PCC_ROM_SLOTS  ((PCC_ROM_END - PCC_ROM_BASE) / PCC_ROM_SLOT)
/* 0xC0000-0xC7FFF is the video BIOS's on every PC, and one built into the
 * system BIOS need not leave a header there to find, so the window starts
 * looking above it. */
#define PCC_WIN_FIRST  0x000C8000u
#define PCC_WIN_SIZE   0x1000u

/* The first 4 KB-aligned, 4 KB host address at or above `from` (and at or
 * above PCC_WIN_FIRST) that no option ROM in `hdr` claims and that ends
 * below PCC_ROM_END. A ROM is a 55 AA header at a 2 KB boundary; it claims
 * from there for its byte 2 in 512-byte units, rounded up to 2 KB, and at
 * least the 2 KB its header is in. -1 when there is none. */
int32_t pcc_attr_pick(const uint8_t *hdr, uint32_t from);

/* A memory range something on the PCI bus decodes, [base, base + len),
 * below 4 GB: a function's memory BAR, or a bridge's window. */
typedef struct {
    uint32_t base;
    uint32_t len;
} pcc_range;

/* Where a socket's registers go when the BIOS left BAR0 unassigned: a 4 KB
 * page in [PCC_BAR_FIRST, PCC_BAR_END), where the APICs begin, above RAM. */
#define PCC_BAR_FIRST  0xF0000000u
#define PCC_BAR_END    0xFEC00000u        /* exclusive */
#define PCC_BAR_SIZE   0x1000u

/* The highest 4 KB page below PCC_BAR_END, at or above PCC_BAR_FIRST and
 * at or above `ram_top` (one past the highest byte of RAM), that none of
 * the `n` ranges in `used` touches. 0 when there is none. */
uint32_t pcc_bar_pick(const pcc_range *used, unsigned n, uint64_t ram_top);

#endif /* FALCON_BIOS_PCCCORE_H */
