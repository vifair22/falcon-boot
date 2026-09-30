/* falcon-bios: a shelf's journal table (falcon-core/src/fs/jbd2.h), at the
 * fixed region biosdefs.h argues for (JNL_REGION).
 *
 * The shelf of an OS that is hibernated, or lost power, is read through its
 * journal, and every read of it goes through the table: its record at the
 * scan (disk/install.c), and the boot's, up to the initrd's last byte
 * (pos/pos.c). The table's scratch block held plaintext from the shelf, so
 * the region is wiped whenever it is given up.
 */

#include <mem/jnlmem.h>

#include <start/bios.h>
#include <mem/e820.h>
#include <crypto/crypto.h>

static size_t g_bytes;   /* of the region in use; 0 when none */

void jnl_wipe(void)
{
    if (g_bytes != 0)
        crypto_wipe((void *)(uintptr_t)JNL_REGION, g_bytes);
    g_bytes = 0;
}

uint64_t jnl_free(void)
{
    uint64_t len = JNL_REGION_END - JNL_REGION;

    return e820_is_ram(JNL_REGION, len) ? len : 0;
}

static void *jnl_get(void *ctx, size_t bytes)
{
    (void)ctx;
    jnl_wipe();
    if (bytes == 0 || bytes > (size_t)(JNL_REGION_END - JNL_REGION) ||
        !e820_is_ram(JNL_REGION, bytes))
        return (void *)0;
    g_bytes = bytes;
    return (void *)(uintptr_t)JNL_REGION;
}

static void jnl_put(void *ctx, void *mem, size_t bytes)
{
    (void)ctx;
    (void)mem;
    (void)bytes;
    jnl_wipe();
}

const fs_jmem jnl_mem = { jnl_get, jnl_put, (void *)0 };
