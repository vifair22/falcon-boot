/* The journal table's memory for the host tools. jmem.h has the contract. */

#include <common/jmem.h>

#include <stdlib.h>

#include <crypto/crypto.h>

static void *g_mem;
static size_t g_len;

void cli_jmem_release(void)
{
    if (g_mem == NULL)
        return;
    crypto_wipe(g_mem, g_len);
    free(g_mem);
    g_mem = NULL;
    g_len = 0;
}

static void *jmem_get(void *ctx, size_t bytes)
{
    (void)ctx;
    cli_jmem_release();
    g_mem = malloc(bytes);
    if (g_mem != NULL)
        g_len = bytes;
    return g_mem;
}

static void jmem_put(void *ctx, void *mem, size_t bytes)
{
    (void)ctx;
    (void)mem;
    (void)bytes;
    cli_jmem_release();
}

const fs_jmem cli_jmem = { jmem_get, jmem_put, NULL };
