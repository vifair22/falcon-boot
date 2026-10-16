/* Block I/O reads through memory below 4 GiB. See biolow.h. */

#include <discover/biolow.h>

#include <lib/string.h>

static biolow_count g_count;
static UINT64 (*g_clock)(void);

void biolow_set_clock(UINT64 (*clock)(void))
{
    g_clock = clock;
}

static void (*g_tick)(void);

void biolow_set_tick(void (*tick)(void))
{
    g_tick = tick;
}

void biolow_counted(biolow_count *out)
{
    *out = g_count;
}

static EFI_STATUS fw_read(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 lba, UINTN len,
                          void *buf)
{
    UINT64 t0 = g_clock != (UINT64 (*)(void))0 ? g_clock() : 0;
    EFI_STATUS es = bio->ReadBlocks(bio, bio->Media->MediaId, lba, len, buf);

    if (g_clock != (UINT64 (*)(void))0)
        g_count.ticks += g_clock() - t0;
    g_count.calls++;
    g_count.bytes += len;
    return es;
}

EFI_STATUS biolow_read(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 lba, UINTN len,
                       void *buf, biolow_fn low, UINTN low_len)
{
    UINT32 bs = bio->Media->BlockSize;
    UINTN chunk = low_len;
    UINT8 *mem;
    UINT8 *dst = (UINT8 *)buf;
    EFI_STATUS es;

    if ((uint64_t)(uintptr_t)buf + len <= BIOLOW_LIMIT)
        return fw_read(bio, lba, len, buf);
    mem = low();
    if (mem == (UINT8 *)0 || bs == 0u || chunk < bs)
        return fw_read(bio, lba, len, buf);
    chunk -= chunk % bs;
    while (len > 0u) {
        UINTN n = len < chunk ? len : chunk;

        es = fw_read(bio, lba, n, mem);
        if (es != EFI_SUCCESS)
            return es;
        memcpy(dst, mem, n);
        if (g_tick != (void (*)(void))0)
            g_tick();
        dst += n;
        lba += n / bs;
        len -= n;
    }
    return EFI_SUCCESS;
}
