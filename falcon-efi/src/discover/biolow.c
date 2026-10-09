/* Block I/O reads through memory below 4 GiB. See biolow.h. */

#include <discover/biolow.h>

#include <lib/string.h>

EFI_STATUS biolow_read(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 lba, UINTN len,
                       void *buf, biolow_fn low, UINTN low_len)
{
    UINT32 bs = bio->Media->BlockSize;
    UINTN chunk = low_len;
    UINT8 *mem;
    UINT8 *dst = (UINT8 *)buf;
    EFI_STATUS es;

    if ((uint64_t)(uintptr_t)buf + len <= BIOLOW_LIMIT)
        return bio->ReadBlocks(bio, bio->Media->MediaId, lba, len, buf);
    mem = low();
    if (mem == (UINT8 *)0 || bs == 0u || chunk < bs)
        return bio->ReadBlocks(bio, bio->Media->MediaId, lba, len, buf);
    chunk -= chunk % bs;
    while (len > 0u) {
        UINTN n = len < chunk ? len : chunk;

        es = bio->ReadBlocks(bio, bio->Media->MediaId, lba, n, mem);
        if (es != EFI_SUCCESS)
            return es;
        memcpy(dst, mem, n);
        dst += n;
        lba += n / bs;
        len -= n;
    }
    return EFI_SUCCESS;
}
