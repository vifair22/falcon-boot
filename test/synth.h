/* Synthetic exFAT volume builder for the unit tests.
 *
 * Builds a complete, byte-accurate exFAT volume in a malloc'd buffer: VBR,
 * FAT, and cluster heap. The point is to reach the driver's error paths,
 * which a real mkfs.exfat image can never produce, and to keep every
 * structural detail a test depends on visible in the test itself rather than
 * buried in a binary fixture.
 *
 * Field offsets follow the Microsoft exFAT specification (August 2019):
 * VBR Section 3.1, directory entries Section 6.x.
 */

#ifndef AIRIES_TEST_SYNTH_H
#define AIRIES_TEST_SYNTH_H

#include <stdint.h>
#include <stddef.h>

#define SYNTH_ENT 32u  /* bytes per directory entry */

typedef struct {
    uint8_t *buf;
    uint64_t len;

    unsigned bps_shift;
    unsigned spc_shift;
    uint32_t fat_offset;     /* sectors */
    uint32_t fat_sectors;
    uint32_t heap_offset;    /* sectors */
    uint32_t cluster_count;
    uint32_t root_cluster;

    /* Fault injection for the read callback. */
    unsigned fail_on_read;   /* 1-based read index; reads at or after it fail.
                                0 disables. */
    int64_t  fail_off;       /* fail any read starting at this offset; -1 off */

    unsigned reads;          /* read callback invocation count */
} synth_vol;

/* Little-endian stores, so a test can patch on-disk fields directly. */
void synth_put_le16(uint8_t *p, uint16_t v);
void synth_put_le32(uint8_t *p, uint32_t v);
void synth_put_le64(uint8_t *p, uint64_t v);

/* Builds a mountable volume: cluster size 1 << (bps_shift + spc_shift), the
 * root directory at cluster 2, and every FAT entry preset to end-of-chain so
 * a single-cluster directory terminates with no per-test setup. */
void synth_init(synth_vol *sv, unsigned bps_shift, unsigned spc_shift,
                uint32_t cluster_count);
void synth_free(synth_vol *sv);

/* exfat_read_fn-compatible. ctx is the synth_vol. */
int synth_read(void *ctx, uint64_t off, uint64_t len, void *buf);

uint32_t synth_cluster_bytes(const synth_vol *sv);
uint8_t *synth_cluster(synth_vol *sv, uint32_t cl);
void synth_fat_set(synth_vol *sv, uint32_t cl, uint32_t val);

/* Individual directory entries, for tests that need a malformed set. */
void synth_put_file(uint8_t *p, uint8_t sec_count, uint16_t attrs);
void synth_put_stream(uint8_t *p, uint8_t flags, uint8_t name_len,
                      uint32_t first_cluster, uint64_t valid_len,
                      uint64_t data_len);
void synth_put_name(uint8_t *p, const char *s, size_t n);
void synth_put_end(uint8_t *p);

/* A well-formed file/directory entry set. Returns the number of 32-byte
 * entries written, so callers can walk a directory cluster linearly. */
unsigned synth_put_set(uint8_t *p, const char *name, uint16_t attrs,
                       uint8_t flags, uint32_t first_cluster, uint64_t size);

#define SYNTH_ATTR_DIR   0x0010u
#define SYNTH_FLAG_ALLOC 0x01u
#define SYNTH_FLAG_NOFAT 0x02u

#endif /* AIRIES_TEST_SYNTH_H */
