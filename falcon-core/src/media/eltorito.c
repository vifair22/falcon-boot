/* The El Torito boot catalog reader. See falcon-core/src/media/eltorito.h for
 * what this module refuses to decide and why. */

#include <media/eltorito.h>

#include <lib/string.h>

/* ---- the boot record volume descriptor (El Torito 1.0 section 2.0) ----
 *
 * Fixed at logical sector 17, which is the first descriptor slot after the
 * primary one at 16. Nothing searches for it: the spec fixes the position,
 * and a scan would only find a "descriptor" in someone's payload data. */
#define BRVD_LBA         17u
#define BRVD_TYPE        0u      /* descriptor type: boot record */
#define BRVD_ID_OFF      1u      /* "CD001" */
#define BRVD_VERSION_OFF 6u      /* 1 */
#define BRVD_SYSTEM_OFF  7u      /* "EL TORITO SPECIFICATION", 32 bytes */
#define BRVD_CATALOG_OFF 0x47u   /* LBA of the boot catalog, LE32 */

static const char iso_magic[5] = { 'C', 'D', '0', '0', '1' };
static const char elt_magic[23] = {
    'E', 'L', ' ', 'T', 'O', 'R', 'I', 'T', 'O', ' ',
    'S', 'P', 'E', 'C', 'I', 'F', 'I', 'C', 'A', 'T', 'I', 'O', 'N'
};

/* ---- the catalog (El Torito 1.0 sections 2.1 and 2.2) ----
 *
 * One 2048-byte sector of 32-byte entries. The first is the validation entry,
 * the second the initial/default entry, and anything after that is a sequence
 * of section headers each followed by the entries it declares. */
#define CAT_ENTRY        32u
#define CAT_ENTRIES      (ELT_SECTOR / CAT_ENTRY)

#define VAL_HEADER_ID    1u      /* byte 0 of the validation entry */
#define VAL_PLATFORM     1u
#define VAL_KEY_OFF      30u     /* 0x55 0xAA */

#define ENT_INDICATOR    0u      /* 0x88 bootable, 0x00 not */
#define ENT_MEDIA        1u      /* low nibble is the media type */
#define ENT_SECTORS      6u      /* LE16, 512-byte units */
#define ENT_LBA          8u      /* LE32, 2048-byte sectors */

#define IND_BOOTABLE     0x88u
#define IND_NOT_BOOTABLE 0x00u

#define HDR_MORE         0x90u   /* a section header, more follow */
#define HDR_FINAL        0x91u   /* the last section header */
#define HDR_EXTENSION    0x44u   /* a section entry extension, not an entry */
#define HDR_PLATFORM     1u
#define HDR_COUNT        2u      /* LE16 number of entries in this section */

/* A read that refuses to go past a known end before asking the callback.
 *
 * `size` of 0 means the caller could not say, and then the callback's own
 * failure is the only bound. That is the honest handling of a medium reached
 * through a pipe-like source rather than a silent success. */
static int read_at(elt_read_fn read, void *ctx, uint64_t size, uint64_t off,
                   uint64_t len, void *buf)
{
    if (size != 0 && (off > size || len > size - off))
        return ELT_ERR_IO;
    return read(ctx, off, len, buf) == 0 ? ELT_OK : ELT_ERR_IO;
}

/* The catalog's bootable entries are recorded in the caller's array until it
 * is full; after that only `truncated` changes. Returning void rather than a
 * status is deliberate: running out of room is not a failure of the medium,
 * and the flag says so without making every call site branch. */
static void add_entry(elt_catalog *out, const uint8_t *e, uint8_t platform,
                      uint8_t is_default)
{
    elt_entry *slot;

    if (out->n >= ELT_MAX_ENTRIES) {
        out->truncated = 1;
        return;
    }
    slot = &out->entry[out->n];
    slot->lba = get_le32(e + ENT_LBA);
    slot->sectors = get_le16(e + ENT_SECTORS);
    slot->platform = platform;
    slot->media = (uint8_t)(e[ENT_MEDIA] & 0x0Fu);
    slot->is_default = is_default;
    out->n++;
}

/* Finds the catalog's sector, or says why there is none. */
static int find_catalog(elt_read_fn read, void *ctx, uint64_t size,
                        uint32_t *lba)
{
    uint8_t sec[ELT_SECTOR];
    int rc;

    rc = read_at(read, ctx, size, (uint64_t)BRVD_LBA * ELT_SECTOR,
                 ELT_SECTOR, sec);
    if (rc != ELT_OK)
        return ELT_ERR_NOT_ISO;
    if (sec[BRVD_TYPE] != 0 ||
        memcmp(sec + BRVD_ID_OFF, iso_magic, sizeof iso_magic) != 0 ||
        sec[BRVD_VERSION_OFF] != 1)
        return ELT_ERR_NOT_ISO;
    /* A boot record that is somebody else's: the slot is generic and the
     * system identifier is what makes it El Torito's. */
    if (memcmp(sec + BRVD_SYSTEM_OFF, elt_magic, sizeof elt_magic) != 0)
        return ELT_ERR_NO_CATALOG;
    *lba = get_le32(sec + BRVD_CATALOG_OFF);
    return ELT_OK;
}

/* Is this sector a validation entry?
 *
 * Header ID and the 0x55 0xAA key are checked; the entry's checksum field is
 * NOT, on purpose. The checksum is a 16-bit sum over the entry that must come
 * to zero, and media exist in the wild that get it wrong while every firmware
 * boots them anyway. Refusing those would be exactly the "stricter than the
 * firmware" failure this module's header argues against, and the fields we
 * then read are each range-checked on their own. */
static int validation_ok(const uint8_t *cat)
{
    return cat[0] == VAL_HEADER_ID &&
           cat[VAL_KEY_OFF] == 0x55u && cat[VAL_KEY_OFF + 1] == 0xAAu;
}

int elt_parse(elt_catalog *out, elt_read_fn read, void *ctx, uint64_t size)
{
    uint8_t cat[ELT_SECTOR];
    uint32_t lba = 0;
    unsigned i;
    uint8_t platform;
    int rc;

    if (out == (elt_catalog *)0)
        return ELT_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (read == (elt_read_fn)0)
        return ELT_ERR_ARG;

    rc = find_catalog(read, ctx, size, &lba);
    if (rc != ELT_OK)
        return rc;
    rc = read_at(read, ctx, size, (uint64_t)lba * ELT_SECTOR, ELT_SECTOR, cat);
    if (rc != ELT_OK) {
        /* The descriptor named a sector the medium does not have. That is a
         * medium claiming a catalog it does not carry, which is the same
         * answer as one whose validation entry is wrong. */
        memset(out, 0, sizeof(*out));
        return ELT_ERR_CORRUPT;
    }
    if (!validation_ok(cat))
        return ELT_ERR_CORRUPT;

    out->catalog_lba = lba;
    out->validation_platform = cat[VAL_PLATFORM];

    /* The initial/default entry at index 1. Its platform is the validation
     * entry's, since a default entry carries no platform field of its own. */
    platform = cat[VAL_PLATFORM];
    if (cat[CAT_ENTRY + ENT_INDICATOR] == IND_BOOTABLE)
        add_entry(out, cat + CAT_ENTRY, platform, 1);

    /* Then section headers, each followed by the entries it declares.
     *
     * i counts 32-byte entries, so every index below is bounded by
     * CAT_ENTRIES and no arithmetic here can leave the sector. A header whose
     * declared count runs off the end is truncated rather than trusted: the
     * count is medium data and a catalog can lie about it. */
    i = 2;
    while (i < CAT_ENTRIES) {
        const uint8_t *hdr = cat + (size_t)i * CAT_ENTRY;
        unsigned count;
        unsigned got;
        int final;

        if (hdr[0] != HDR_MORE && hdr[0] != HDR_FINAL)
            break;
        final = hdr[0] == HDR_FINAL;
        platform = hdr[HDR_PLATFORM];
        count = get_le16(hdr + HDR_COUNT);
        i++;

        for (got = 0; got < count && i < CAT_ENTRIES; i++) {
            const uint8_t *e = cat + (size_t)i * CAT_ENTRY;

            /* An extension carries more of the previous entry's data and is
             * not an entry itself, so it does not count against the section's
             * declared total. */
            if (e[ENT_INDICATOR] == HDR_EXTENSION)
                continue;
            if (e[ENT_INDICATOR] == IND_BOOTABLE)
                add_entry(out, e, platform, 0);
            else if (e[ENT_INDICATOR] != IND_NOT_BOOTABLE)
                break;  /* not an entry at all; the section is malformed */
            got++;
        }
        if (got < count)
            out->truncated = 1;
        if (final)
            break;
    }
    return ELT_OK;
}

const elt_entry *elt_bios_entry(const elt_catalog *cat)
{
    unsigned i;

    if (cat == (const elt_catalog *)0)
        return (const elt_entry *)0;
    for (i = 0; i < cat->n; i++) {
        if (cat->entry[i].is_default &&
            cat->entry[i].platform != ELT_PLATFORM_UEFI)
            return &cat->entry[i];
    }
    for (i = 0; i < cat->n; i++) {
        if (cat->entry[i].platform == ELT_PLATFORM_X86)
            return &cat->entry[i];
    }
    return (const elt_entry *)0;
}
