/* crc32c, reflected, nibble table: 64 bytes of rodata and two lookups per
 * byte. Nothing here is on a hot path (ext4 checksums metadata only, never
 * file data, and the journal is read once per mount), so the 1 KiB byte-wide
 * table would buy nothing.
 */

#include <fs/crc32c.h>

/* Entry i is the reflected polynomial 0x82F63B78 folded four times over i. */
static const uint32_t crc32c_tab[16] = {
    0x00000000U, 0x105EC76FU, 0x20BD8EDEU, 0x30E349B1U,
    0x417B1DBCU, 0x5125DAD3U, 0x61C69362U, 0x7198540DU,
    0x82F63B78U, 0x92A8FC17U, 0xA24BB5A6U, 0xB21572C9U,
    0xC38D26C4U, 0xD3D3E1ABU, 0xE330A81AU, 0xF36E6F75U,
};

uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc32c_tab[crc & 0xFu];
        crc = (crc >> 4) ^ crc32c_tab[crc & 0xFu];
    }
    return crc;
}

uint32_t crc32c_zeros(uint32_t crc, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        crc = (crc >> 4) ^ crc32c_tab[crc & 0xFu];
        crc = (crc >> 4) ^ crc32c_tab[crc & 0xFu];
    }
    return crc;
}
