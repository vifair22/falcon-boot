/* falcon-efi: a shelf's journal table (falcon-core/src/fs/jbd2.h), from the
 * firmware as pages.
 *
 * The shelf of an OS that is hibernated, or lost power, is read through its
 * journal, and every read of it goes through the table: its record at the
 * scan (discover/discover.c), and the boot's, until the UKI is in memory
 * (pos/pos.c). The table's scratch block held plaintext from the shelf, so
 * it is wiped before the pages go back.
 */

#include <mem/jnlmem.h>

#include <crypto/crypto.h>

static EFI_BOOT_SERVICES   *g_bs;
static EFI_PHYSICAL_ADDRESS g_addr;
static UINTN                g_pages;

void jnl_release(void)
{
    if (g_pages == 0)
        return;
    crypto_wipe((void *)(uintptr_t)g_addr, (size_t)g_pages * EFI_PAGE_SIZE);
    (void)g_bs->FreePages(g_addr, g_pages);
    g_pages = 0;
}

static void *jnl_get(void *ctx, size_t bytes)
{
    EFI_BOOT_SERVICES *bs = ctx;
    UINTN pages = (UINTN)((bytes + EFI_PAGE_SIZE - 1u) / EFI_PAGE_SIZE);

    jnl_release();
    if (bs == (EFI_BOOT_SERVICES *)0 || pages == 0 ||
        bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &g_addr) !=
            EFI_SUCCESS)
        return (void *)0;
    g_bs = bs;
    g_pages = pages;
    return (void *)(uintptr_t)g_addr;
}

static void jnl_put(void *ctx, void *mem, size_t bytes)
{
    (void)ctx;
    (void)mem;
    (void)bytes;
    jnl_release();
}

void jnl_mem_make(fs_jmem *m, EFI_BOOT_SERVICES *bs)
{
    m->get = jnl_get;
    m->put = jnl_put;
    m->ctx = bs;
}
