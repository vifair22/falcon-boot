/* falcon jump's drivers: INT 13h for one block device. See blk13.h. */

#include <hw/blk13.h>

#include <lib/string.h>

/* The heads and sectors per track AH=08h reports for a disk addressed by
 * LBA, which is what every BIOS of the era reports for a large one. Only a
 * caller that insists on CHS uses them. */
#define FAKE_HEADS   255u
#define FAKE_SECTORS 63u

/* The BDA's count of fixed disks, which AH=08h answers in DL: DOS takes it
 * as how many drives from 0x80 up to ask about. The route raised it for
 * this disk before handing over. */
#define BDA_NDISKS 0x475u

static uint8_t g_last;

/* Whether the drive is answered as a fixed disk. A CF card or a stick
 * reports removable media, but served under a hard disk's number (0x80 up,
 * below the CD numbers) it is answered as BIOSes answer a hard disk: with a
 * removable answer to AH=15h, DOS asks AH=16h for a media change before
 * its reads, takes the failure as one, and drops what it has read. */
static int fixed(const blk13_dev *d, const jmod_regs *r)
{
    uint8_t dl = (uint8_t)(r->edx & 0xFFu);

    return !d->removable || (dl >= 0x80u && dl < 0xE0u);
}

/* A byte of low memory, read the way start/bios.h's peek8 does, so the
 * compiler takes no view on an object at a small constant address. */
static uint8_t low8(uint32_t addr)
{
    uint8_t v;

    __asm__ __volatile__("movb (%1), %0" : "=q"(v) : "r"(addr) : "memory");
    return v;
}

static uint8_t *linear(uint16_t seg, uint16_t off)
{
    return (uint8_t *)(uintptr_t)(((uint32_t)seg << 4) + off);
}

static void answer(jmod_regs *r, uint8_t ah, int fail)
{
    r->eax = (r->eax & 0xFFFF00FFu) | ((uint32_t)ah << 8);
    r->flags = fail ? 1u : 0u;
    g_last = ah;
}

/* AH=42h and AH=43h: the packet at DS:SI, and on a short transfer the count
 * written back as what was done. */
static void extended(const blk13_dev *d, jmod_regs *r, int write)
{
    uint8_t *p = linear(r->ds, (uint16_t)r->esi);
    uint16_t count;
    uint16_t done = 0;
    uint64_t lba;
    uint8_t *buf;

    memcpy(&count, p + 2, 2);
    memcpy(&lba, p + 8, 8);
    buf = linear((uint16_t)((unsigned)p[6] | ((unsigned)p[7] << 8)),
                 (uint16_t)((unsigned)p[4] | ((unsigned)p[5] << 8)));
    if (write && d->write == 0) {
        answer(r, 0x03u, 1);                   /* write protected */
        return;
    }
    while (done < count) {
        uint32_t n = (uint32_t)(count - done);
        uint8_t *at = buf + (uint32_t)done * d->sector_size;
        int rc;

        if (n > d->max_count)
            n = d->max_count;
        rc = write ? d->write(lba + done, n, at) : d->read(lba + done, n, at);
        if (rc != 0)
            break;
        done = (uint16_t)(done + n);
    }
    memcpy(p + 2, &done, 2);
    answer(r, done == count ? 0 : (write ? 0xCCu : 0x04u), done != count);
}

/* AH=02h and AH=03h: CHS against the reported geometry, one sector at a
 * time, into ES:BX. */
static void chs(const blk13_dev *d, jmod_regs *r, int write)
{
    uint32_t count = r->eax & 0xFFu;
    uint32_t cyl = ((r->ecx >> 8) & 0xFFu) | ((r->ecx & 0xC0u) << 2);
    uint32_t sec = r->ecx & 0x3Fu;
    uint32_t head = (r->edx >> 8) & 0xFFu;
    uint8_t *buf = linear(r->es, (uint16_t)r->ebx);
    uint64_t lba;
    uint32_t i;

    if (sec == 0 || count == 0 || (write && d->write == 0)) {
        answer(r, write ? 0x03u : 0x01u, 1);
        return;
    }
    lba = ((uint64_t)cyl * FAKE_HEADS + head) * FAKE_SECTORS + (sec - 1u);
    for (i = 0; i < count; i++) {
        uint8_t *at = buf + i * d->sector_size;
        int rc = write ? d->write(lba + i, 1u, at) : d->read(lba + i, 1u, at);

        if (rc != 0) {
            r->eax = (r->eax & 0xFFFFFF00u) | i;
            answer(r, 0x04u, 1);
            return;
        }
    }
    answer(r, 0, 0);
}

void blk13_call(const blk13_dev *d, jmod_regs *r)
{
    uint8_t ah = (uint8_t)((r->eax >> 8) & 0xFFu);
    uint8_t *p;

    switch (ah) {
    case 0x00:                                 /* reset */
        answer(r, 0, 0);
        return;
    case 0x01:                                 /* last status */
        answer(r, g_last, 0);
        return;
    case 0x02:
    case 0x03:
        chs(d, r, ah == 0x03);
        return;
    case 0x08: {                               /* geometry */
        uint64_t cyls = d->sectors / ((uint64_t)FAKE_HEADS * FAKE_SECTORS);

        if (cyls > 1024u)
            cyls = 1024u;
        if (cyls == 0)
            cyls = 1u;
        cyls -= 1u;
        r->ecx = (r->ecx & 0xFFFF0000u) | ((uint32_t)(cyls & 0xFFu) << 8) |
                 (uint32_t)((cyls >> 2) & 0xC0u) | FAKE_SECTORS;
        r->edx = (r->edx & 0xFFFF0000u) | ((FAKE_HEADS - 1u) << 8) |
                 low8(BDA_NDISKS);
        answer(r, 0, 0);
        return;
    }
    case 0x15: {                               /* drive type */
        uint32_t n = d->sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu
                                              : (uint32_t)d->sectors;

        if (!fixed(d, r)) {
            answer(r, 0x02u, 0);
            return;
        }
        r->ecx = (r->ecx & 0xFFFF0000u) | (n >> 16);
        r->edx = (r->edx & 0xFFFF0000u) | (n & 0xFFFFu);
        answer(r, 0x03u, 0);
        return;
    }
    case 0x41:                                 /* extensions present? */
        if ((r->ebx & 0xFFFFu) != 0x55AAu) {
            answer(r, 0x01u, 1);
            return;
        }
        r->ebx = (r->ebx & 0xFFFF0000u) | 0xAA55u;
        r->ecx = (r->ecx & 0xFFFF0000u) | 0x0001u;   /* packet calls */
        answer(r, 0x21u, 0);
        g_last = 0;
        return;
    case 0x42:
    case 0x43:
        extended(d, r, ah == 0x43);
        return;
    case 0x48: {                               /* drive parameters */
        uint16_t size;
        uint16_t v;
        uint32_t z = 0;
        uint64_t n = d->sectors;

        p = linear(r->ds, (uint16_t)r->esi);
        memcpy(&size, p, 2);
        if (size < 0x1Au) {
            answer(r, 0x01u, 1);
            return;
        }
        v = 0x1Au;
        memcpy(p, &v, 2);
        v = fixed(d, r) ? 0x0000u : 0x0004u;
        memcpy(p + 2, &v, 2);
        memcpy(p + 4, &z, 4);
        memcpy(p + 8, &z, 4);
        memcpy(p + 12, &z, 4);
        memcpy(p + 16, &n, 8);
        v = (uint16_t)d->sector_size;
        memcpy(p + 24, &v, 2);
        answer(r, 0, 0);
        return;
    }
    default:
        answer(r, 0x01u, 1);                   /* invalid function */
        return;
    }
}
