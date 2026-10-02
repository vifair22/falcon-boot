/* falcon jump: what a module is, to the core that loads it and to the module
 * itself (docs/DESIGN.md Section 13.4).
 *
 * A module is a driver linked at a fixed slot (biosdefs.h, JMOD_SLOT_ADDR),
 * read off the floppy into it, and entered through its init. It begins with
 * a header the core checks before a single byte of it runs: a stale module
 * from another build, or one copied under the wrong name, is refused with a
 * sentence rather than jumped into.
 *
 * TWO TABLES, AND WHEN EACH MAY BE USED. The core hands init a table of
 * services (serial output) and the module fills in a table of operations.
 * The services exist only while the core does: once the medium's own
 * bootloader is running, the core is gone and so is everything in that
 * table. So jmod_ops.int13 and int16, the resident entries, use nothing but
 * the module itself - its own port I/O, its own delays, its own buffers.
 * probe, dev, read and serve run while the core is there and may log.
 */

#ifndef FALCON_BIOS_JMOD_H
#define FALCON_BIOS_JMOD_H

#include <stddef.h>
#include <stdint.h>

#define JMOD_MAGIC 0x444D4A46u   /* "FJMD" */
#define JMOD_ABI   3u

/* The header, at the slot's first byte. Offsets rather than a struct on the
 * core's side, so the check reads the same bytes whatever compiler reads
 * them; a module declares it with jmod_header_decl below. */
#define JMOD_OFF_MAGIC     0x00   /* u32 */
#define JMOD_OFF_ABI       0x04   /* u16 */
#define JMOD_OFF_HDR_LEN   0x06   /* u16 */
#define JMOD_OFF_SLOT      0x08   /* u32: the address it is linked at */
#define JMOD_OFF_IMAGE_END 0x0C   /* u32: one past the file's last byte */
#define JMOD_OFF_MEM_END   0x10   /* u32: one past its .bss */
#define JMOD_OFF_INIT      0x14   /* u32: jmod_init_fn */
#define JMOD_OFF_NAME      0x18   /* 8 bytes, NUL-padded */
#define JMOD_HDR_LEN       0x20

typedef enum {
    JMOD_OK          =  0,
    JMOD_ERR_ARG     = -1,
    JMOD_ERR_MAGIC   = -2,   /* not a module */
    JMOD_ERR_ABI     = -3,   /* a module from another build */
    JMOD_ERR_SLOT    = -4,   /* linked for another slot */
    JMOD_ERR_SIZE    = -5,   /* the file and the header disagree, or it
                                does not fit its slot with its stack */
    JMOD_ERR_ENTRY   = -6    /* init is not inside the image */
} jmod_status;

typedef struct {
    uint32_t slot;
    uint32_t image_len;      /* bytes of the file, all read to the slot */
    uint32_t mem_len;        /* with .bss, which the core zeroes */
    uint32_t init;
    char     name[9];
} jmod_info;

/* Checks the first `have` bytes of a module file of `file_len` bytes, meant
 * for the slot at `slot` of `slot_bytes`, and fills `out`. */
jmod_status jmod_check(const uint8_t *hdr, size_t have, uint64_t file_len,
                       uint32_t slot, uint32_t slot_bytes, jmod_info *out);

/* ---- the calls between core and module, in protected mode ---- */

typedef struct {
    uint32_t abi;
    void (*log)(const char *s);
    void (*log_hex)(uint64_t v);
} jmod_services;

#define JMOD_DEV_CD   1u
#define JMOD_DEV_DISK 2u

typedef struct {
    uint32_t kind;           /* JMOD_DEV_* */
    uint32_t sector_size;
    uint64_t sectors;        /* 0 when there is no medium */
    char     label[41];      /* the device's model string */
} jmod_dev;

/* The registers of an INT 13h call, as the resident stub saved them and as
 * it restores them. `flags` carries the carry bit out. */
typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esi;
    uint32_t edi;
    uint16_t ds;
    uint16_t es;
    uint16_t flags;
    uint16_t pad;
} jmod_regs;

typedef struct {
    /* How many devices the module found; the core's list. */
    unsigned (*probe)(void);
    int (*dev)(unsigned idx, jmod_dev *out);
    /* `count` sectors of the device's own size from `lba` into `buf`. */
    int (*read)(unsigned idx, uint64_t lba, uint32_t count, void *buf);
    /* The device the resident answers for, as BIOS drive `drive`. */
    int (*serve)(unsigned idx, uint8_t drive);
    /* The resident entry: an INT 13h call for that drive. */
    void (*int13)(jmod_regs *r);
    /* The resident's keyboard entry (DESIGN Section 13.6): polls the
     * keyboards probe found and puts their keys into the BIOS's ring and
     * shift flags. probe fills it in when it found one; 0 otherwise. */
    void (*int16)(jmod_regs *r);
    /* What probe took from the machine, given back: a socket powered down,
     * its windows closed. The route calls it when it comes back without a
     * handover. 0 when there is nothing to give back (the USB modules keep
     * their keyboards up, DESIGN Section 13.6). */
    void (*release)(void);
} jmod_ops;

typedef int (*jmod_init_fn)(const jmod_services *svc, jmod_ops *ops);

#endif /* FALCON_BIOS_JMOD_H */
